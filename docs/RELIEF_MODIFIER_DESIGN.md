# Relief Modifier — Painter-Driven Shading-Normal Micro-Relief, and the Deprecation of `bumpmap_modifier`

**Status:** Phases 1–5 landed and reviewed to zero P1 (2026-09-06): Phase 1
four rounds; Phase 2 one round + fix; Phase 3 three lenses + fix + converge;
Phase 4 two fix rounds (`177bcee4`, `e5d0fa0b`) then a converging round with
zero P1; Phase 5 reviewed with Phase 4. Record in §12. Each phase ran the
[implementation-review-loop](skills/implementation-review-loop.md) to zero
P1 before the next starts. The per-phase record is appended to §12 as
phases land.
**Branch:** `relief-modifier` off `master` at `2cf923b7`.
**Inputs (verified in tree, 2026-09-05):** the three existing modifiers
([BumpMap.cpp](../src/Library/Modifiers/BumpMap.cpp),
[NormalMap.cpp](../src/Library/Modifiers/NormalMap.cpp),
[GlintModifier.cpp](../src/Library/Modifiers/GlintModifier.cpp)) and their
descriptors (`BumpmapModifierAsciiChunkParser`, `NormalMapModifierAsciiChunkParser`,
`GlintModifierAsciiChunkParser`, all in [ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp));
the hit record ([RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h));
the modifier hook and world-space promotion in
[Object::IntersectRay through its `ri.pModifier = pModifier` assignment](../src/Library/Objects/Object.cpp); the physical-scalar pipe
([IScalarPainter.h](../src/Library/Interfaces/IScalarPainter.h),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md)) and its resolver
`ResolveOrDiagnoseScalar` (a static helper in [Job.cpp](../src/Library/Job.cpp)); the
expression VM context (`LookupContextVar` in [ExpressionEval.h](../src/Library/Painters/ExpressionEval.h),
`ExpressionPainter::BuildContext` in [ExpressionPainter.cpp](../src/Library/Painters/ExpressionPainter.cpp));
the filter-width plumbing ([TextureFootprintCompute.h](../src/Library/Intersection/TextureFootprintCompute.h),
`OctaveFadeWeightImpl` and its `Fbm3D`/`Turbulence3D`/`Ridged3D` consumers in [ProceduralNoiseCore.cpp](../src/Library/Utilities/ProceduralNoiseCore.cpp));
the copy-then-offset precedent in `MappingPainter::GetColor`'s per-`projection`-case copy ([MappingPainter.cpp](../src/Library/Painters/MappingPainter.cpp));
the adoption laws in `docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md`
§2 and §7; the verb machinery in [AgentSession.cpp](../src/Library/Agent/AgentSession.cpp)
(`AddWear` ~36946, `AddWetness` ~37340); the migrator precedents
`tools/migrate_scenes_light_colorspace.py` / `tools/migrate_scenes_iscalarpainter.py`;
[CstDeriveGoldenTest.cpp](../tests/CstDeriveGoldenTest.cpp).

---

## 1. The question, and the answer

RISE's procedural texture stack authors fields in three dimensions. An
`expression_painter` body reads `P`, `Po`, `N`, `fw`, `curv`, `occlusion()`;
`voronoi3d_painter`, `mapping_painter` (world / object / triplanar), the
stochastic-tiling and ramp painters all evaluate at the hit point, on any
geometry, with no texcoords required. Those fields already drive albedo
(`IPainter`) and every microsurface scalar (`IScalarPainter`). They cannot
drive the shading normal.

The only two normal-perturbing modifiers are `bumpmap_modifier`, which
samples an `IFunction2D` at `ri.ptCoord` (UV only; a colour painter bound
there is evaluated through `Painter::Evaluate`, which builds a fake hit with
*only* `ptCoord` set, clamps UV to [0,1], and returns the R channel —
[Painter.cpp](../src/Library/Painters/Painter.cpp)), and
`normal_map_modifier`, which decodes an image. So the grain field that
darkens and roughens the workbench top cannot also tilt its normal, and
authored variation reads as paint on plastic. The flagship in-tree bump user
says exactly this about itself: *"Under directional lights alone the cushion
renders as dark glossy plastic — the look this material exists to replace"*
(the gate-9 `velvet_cushion` finding "A velvet needs a DOME, and a LOW rim" in
[CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md)).

**The answer** is a fourth modifier, `relief_modifier`, whose height is *any*
`IScalarPainter` (or any colour painter through the existing
`scalar_painter { painter X channel R }` bridge), evaluated as a 3D field by
central difference in the tangent plane at the hit, with the step chosen from
the pixel footprint so relief fades to flat at distance the same way the
noise builtins fade their octaves. It needs no texcoords. It composes with
`normal_map_modifier` and `glint_modifier` through a new, trivial
`modifier_stack` chunk (the object slot is a single pointer today — §4).
`bumpmap_modifier` is deprecated with a parse-time diagnostic now and a
lossless migrator; its removal is a named later phase (§7).

### 1.1 Three charter premises corrected against source

The charter that opened this arc stated three things the tree does not bear
out. Recording them so the design does not inherit them:

1. **"`displaced_geometry` already accepts any Painter."** It does not. Its
   `displacement` slot is *declared* `{ChunkCategory::Painter}` but resolved
   through `pFunc2DManager` (`Job::AddDisplacedGeometry` in [Job.cpp](../src/Library/Job.cpp)) and
   evaluated as `displacement.Evaluate(u, v)` per vertex
   (`ApplyDisplacementMapToObject` in [GeometryUtilities.cpp](../src/Library/Geometry/GeometryUtilities.cpp)).
   A 3D `expression_painter` bound there evaluates through the fake-hit
   `Painter::Evaluate` path and is a *constant*. The "same field drives coarse
   displacement + fine relief" pattern therefore works today only for
   UV-domain fields (§5.3). Extending `displaced_geometry` to evaluate an
   `IScalarPainter` at the vertex position is a follow-up, chipped, not in
   this arc.
2. **"Trajectory analyses in `docs/agentic-redesign/` flagged this
   repeatedly."** No file in that directory mentions bump, relief, or
   "reads as paint" (exhaustive case-insensitive grep, 36 files). The
   motivation is real but its in-tree evidence is
   [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) (§3658-3675, §522-524) and
   the absence of *any* teaching-surface route to normal perturbation
   (`skills/` has zero hits for `bumpmap`/`normal_map_modifier`; the only
   relief recipe routes agents to `displaced_geometry`, the
   `expression_function2d` row of the pattern-selection table in
   [procedural-textures.md](../skills/agent/procedural-textures.md)).
3. **"Env domes remain `expression_function2d`'s legitimate niche."** Not
   evidenced: all 18 in-tree `expression_function2d` scenes bind it to
   `displaced_geometry.displacement`, `function2d_painter`, or
   `scalar_painter { function2d }`; every env dome in tree is a
   `uniformcolor_painter`/`blend_painter`/`hdr_painter` chain. Its surviving
   niche after this arc is **displacement** (§7.4).

---

## 2. What exists, precisely

| Surface | Fact | Where |
|---|---|---|
| Modifier interface | one method, `Modify(RayIntersectionGeometric&) const` | [IRayIntersectionModifier.h](../src/Library/Interfaces/IRayIntersectionModifier.h) |
| Hook timing | `Object::IntersectRay` promotes `vNormal` and `vGeomNormal` to world space, builds `onb` (via `CreateFromWU` when **`bShadingTangentFromGeometry`** — from the geometry's own tangent in the `bHasShadingTangent` sub-case, from a world-X projection otherwise, e.g. SDFGeometry's heightfield mode — else `CreateFromW`; **the original text here named `bHasShadingTangent` as the branch condition, which is the sub-case, not the branch — the error that produced fix round 1's P1-A**), fills `derivatives`, `txFootprint`, `signals`, then assigns `ri.pModifier`. `Modify` fires at every *consumer* (RayCaster, PT, BDPT eye/light walks, photon tracers, SMS, SSS, AOVs — ~25 sites) immediately after the cast, before any material call. | [`Object::IntersectRay`, through its `ri.pModifier = pModifier` assignment](../src/Library/Objects/Object.cpp), the `ri.pModifier->Modify(...)` call in `RayCaster::CastRay` ([RayCaster.cpp](../src/Library/Rendering/RayCaster.cpp)) |
| Geometric normal | captured before the modifier and never touched by it; every SPF/BRDF's geometric-horizon gate compares against it with the `SquaredModulus > 1e-12` degeneracy guard | the `vGeomNormal` field comment in [RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h), the matching guard in [GGXSPF.cpp](../src/Library/Materials/GGXSPF.cpp) and [DielectricSPF.cpp](../src/Library/Materials/DielectricSPF.cpp) |
| BDPT / VCM | `Modify` runs once per surface vertex, then `normal`, `geomNormal`, `onb` are frozen into `BDPTVertex`; VCM reuses that record via `PopulateRIGFromVertex`. No integrator has a bump-terminator correction; none needs a change for a new modifier. | the eye/light-vertex population in [BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp), the `normal`/`geomNormal`/`onb` fields in [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h) |
| Frame rebuild | all three pre-existing modifiers project the *current* `onb.u()` onto the new normal's plane, `CreateFromWU`, and restore incoming handedness with `FlipV` (the mirrored-instance fix); fall back to `CreateFromW` if the projection degenerates. Bump/NormalMap run the projection only on a coherent-tangent hit; Glint runs it unconditionally — see §12 deviation 3 and fix round 1 P1-A for the gate | the gated `ModifierFrame::RebuildPreservingTangent` call in [`NormalMap::Modify`](../src/Library/Modifiers/NormalMap.cpp), the unconditional `ModifierFrame::RebuildPreservingTangent` call at the end of [`GlintModifier::Modify`](../src/Library/Modifiers/GlintModifier.cpp) |
| One modifier per object | `Object::pModifier` is a single pointer; `AssignModifier` replaces; a CSG composite's own modifier *overrides* the child's. A 2002 comment says "this should be a list of some sort... eventually". (Composition via `modifier_stack` since Phase 2.) | the `pModifier` field in `class Object` ([Object.h](../src/Library/Objects/Object.h)), [the `if( pModifier )` override block in `CSGObject::IntersectRay`](../src/Library/Objects/CSGObject.cpp), the `pModifier` field's "this should be a list of somesort... eventually" comment in [RayIntersection.h](../src/Library/Intersection/RayIntersection.h) |
| Scalar pipe | `IScalarPainter::GetValuesAt(ri)` → `ScalarTriple`; single-scalar slots read `.v[0]` and the resolver rejects per-channel painters when `requireSingle`; an `IPainter` name bound to a scalar slot gets `kScalarBoundToIPainterFmt` | the `GetValuesAt`/`GetValueAtNM` contract comment in [IScalarPainter.h](../src/Library/Interfaces/IScalarPainter.h), `ResolveOrDiagnoseScalar` in [Job.cpp](../src/Library/Job.cpp), `kScalarBoundToIPainterFmt` in [ChunkDescriptor.h](../src/Library/Parsers/ChunkDescriptor.h) |
| Any-painter bridge | `scalar_painter { painter X channel R\|G\|B\|A [scale] [bias] }` → `PainterChannelScalarPainter`; `scalar_painter { function2d F }` → `Function2DScalarPainter` (evaluates `F.Evaluate(ptCoord)` — the *same* sampling path `bumpmap_modifier` uses today) | the `ScalarPainterAsciiChunkParser` `painter`/`function2d` cases in [ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp) |
| Filter width | `ri.txFootprint.worldWidth` (Igehy ray differentials), populated by triangle-mesh geometry only; `fw = 0` on analytic primitives. The expression VM's `perlin/fbm/turbulence/ridged` fade octaves against it (smoothstep, resolved below 0.2, faded at 0.6; abs-based noises fade to their measured mean) | [TextureFootprintCompute.h](../src/Library/Intersection/TextureFootprintCompute.h), `OctaveFadeWeightImpl` and its `Turbulence3D`/`Ridged3D` consumers in [ProceduralNoiseCore.cpp](../src/Library/Utilities/ProceduralNoiseCore.cpp) |
| Evaluate-elsewhere idiom | `RayIntersectionGeometric ri2 = ri; ri2.ptIntersection = ...; source.GetColor(ri2)` — `MappingPainter` does exactly this for world/object/UV remaps and invalidates `txFootprint` only when the UV *domain* is remapped | `MappingPainter::GetColor` in [MappingPainter.cpp](../src/Library/Painters/MappingPainter.cpp) |
| Space semantics | `Proj_World` reads `ptIntersection`; `Proj_Object` and `voronoi3d space object` read `ptObjIntersec`; UV painters read `ptCoord`; triplanar reads `ptIntersection` + `vNormal` | the `Projection` enum in [MappingPainter.h](../src/Library/Painters/MappingPainter.h), its `Proj_*` cases in `MappingPainter::GetColor` |
| Deprecation conventions | no `ChunkDescriptor::deprecated` field exists. Three precedents: (a) *removed* → generic `kUndeclaredParameterFmt` / unknown-chunk hard fail (`branching_threshold`, BDPT `sms_*`); (b) *accepted-and-ignored* → declared with `"Legacy — ignored"` (`branch`); (c) *deprecated-with-prose* → description prefixed `"DEPRECATED (...)"` (`lights_intensity_override`). Description text flows verbatim into the agent tool schema (`SchemaGen.cpp`) and the GUI suggestion surfaces. | `lights_intensity_override`'s descriptor; the `branch` `"Legacy — ignored"` param in `PathTracingShaderOpAsciiChunkParser::Describe`; the `optimal_mis*` omission comment in `BDPTPelRasterizerAsciiChunkParser::Describe` (all in [ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)) |
| ABI freeze | `IJob::AddBumpMapModifier(name, func, scale, window)` is signature-frozen; the sole out-of-tree caller is the `job.AddBumpMapModifier(...)` call in [rise_blender_bridge.cpp](../src/Blender/native/rise_blender_bridge.cpp) | the `AddBumpMapModifier` declaration in [IJob.h](../src/Library/Interfaces/IJob.h) |
| In-tree bump users | 4 scenes: `velvet_cushion` (expression_function2d, normalized), `sculptors_studio` (perlin2d_painter), `sms_veach_egg_bumpmap` (perlin2d_painter), `Internal/pool` (checker + an unreferenced png bump; gitignored). 4 CST tests embed `bumpmap_modifier` literals. No ChunkCoverage scene. | §7.3 |

---

## 3. The modifier

### 3.1 Scene language

```
relief_modifier
{
	name     wood_relief
	height   sp_wood_height        # scalar_painter (or inline numeric -> flat, pointless)
	scale    0.004                 # height amplitude: field units -> world units (surface) / UV units (uv)
	domain   surface               # surface (default) | uv
	step     0                     # 0 = auto (surface: max(1e-3, fw); uv: 0.01)
}
```

- `height` — `ValueKind::Reference`, `referenceCategories {Painter}`,
  `semantics.pipe = ParameterPipe::Scalar`, `requireSingle = true`. Resolved
  through `ResolveOrDiagnoseScalar`, so a colour painter bound directly
  produces the standing `kScalarBoundToIPainterFmt` diagnostic — which the
  descriptor text and the diagnostic's follow-on sentence turn into the
  one-line fix: *wrap it: `scalar_painter { name X_h painter X channel R }`*.
  Height is a physical length, not a colour; it must never pass through JH
  uplift, so it lives on the scalar pipe by the routing rule in
  [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md). Spectral
  renderers read `GetValuesAt(ri).v[0]` (height is wavelength-independent by
  construction; `requireSingle` guarantees the triple is uniform).
- `scale` — the amplitude multiplier. Positive height **rises along +N**
  (Blinn 1978; PBRT-v4 §10.3 convention). Note this is the *opposite* sign of
  the legacy modifier, whose `vNormal + T·Δf` tilts the normal *toward* the
  rise, i.e. treats `f` as depth. The migrator folds the sign (§7.2).
- `domain surface` (default) — the field is a function of the 3D hit and the
  step is taken in the tangent plane in **world units**. Works on any
  geometry with a normal; texcoords not required.
- `domain uv` — the field is a function of `(u, v)` and the step is taken in
  texture units along `onb.u()/onb.v()` exactly as `bumpmap_modifier` does.
  Exists so migration is lossless (§7.2) and for image heightfields authored
  in UV; the descriptor says so and says `surface` is the recommended mode.
- `step` — central-difference half-step. `0` selects the automatic rule in
  §3.3.

### 3.2 The perturbation

With `T = onb.u()`, `B = onb.v()`, `N = vNormal` (already the frame the
material will shade in, geometry-supplied tangent honoured), for
`domain surface`:

```
h_T = ( H(P + s·T) − H(P − s·T) ) / (2s)
h_B = ( H(P + s·B) − H(P − s·B) ) / (2s)
N'  = normalize( N − scale·( h_T·T + h_B·B ) )
```

`N' = N − h_u t − h_v b` is the exact normal of `p' = p + h·n` to first order
in `h` for an orthonormal `(t, b, n)`; it holds for either handedness (the
overall sign of `t'×b'` flips, the perturbation relative to `n` does not) and
the tangent-plane gradient of a 3D field is frame-independent, so the
arbitrary `CreateFromW` tangent on tangent-less geometry is *correct*, not a
compromise — the orientation caveat that limits `bumpmap_modifier` and the UV
domain does not apply.

**Evaluating `H` at an offset point** follows the `MappingPainter` idiom: copy
`ri`, offset the point fields, call `GetValuesAt`. Every point domain a
painter can read must move consistently, or a painter in that domain sees a
flat field:

- `ptIntersection` — moved by the world-space step. Read by world-space and
  triplanar painters and by `P` in expressions.
- `ptObjIntersec` — moved by the step mapped through the object's
  world→object linear map. Read by object-space painters (`mapping_painter
  space object`, `voronoi3d space object`, `Po` in expressions). The hit
  record does not carry the transform today; **§3.4 adds it.**
- `ptCoord` — when `ri.derivatives.valid`, moved by the least-squares
  solution of `dpdu·Δu + dpdv·Δv = Δp` (exact for the local linear map), so a
  UV-parameterised painter (image, `perlin2d`, `checker`) still yields the
  right gradient in `surface` mode. When derivatives are absent, `ptCoord` is
  left unchanged and such a painter reads flat — the descriptor names
  `domain uv` as the route for that case.
- `ptCoord1` (TEXCOORD_1) — **not moved, in either domain.** The surface
  domain's chain rule has only TEXCOORD_0's `dpdu`/`dpdv` to invert, and the
  UV domain steps `ptCoord` alone. A height parameterised on the second UV
  set (`texcoord1_painter`) therefore reads flat and yields no relief
  anywhere; it is not supported. Author the height against TEXCOORD_0, or use
  a 3D field in `surface`. The descriptor says so on the `height` parameter.
- `vNormal`, `signals`, `derivatives`, `txFootprint` — unchanged. Triplanar
  blending weights, `curv`, `occlusion()`, `thickness()` are treated as
  locally constant, so a height expression that *is* `curv` produces zero
  relief; that is the documented semantics, not a bug (the signals are
  geometry-derived, and the design of that arc says they are invariant under
  shading-normal modifiers — [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)).
  `txFootprint` is *kept valid* deliberately: the four offset evaluations
  describe the same pixel footprint, so an `fbm` height fades its octaves at
  the offsets exactly as at the centre, and the gradient of a fully-faded
  field is zero — relief inherits the fade-to-mean discipline for free.

For `domain uv` the step is applied to `ptCoord` only, the divisor is the UV
span, and the tangents are `onb.u()/onb.v()` — byte-for-byte the legacy
sampling geometry, so §7.2's algebra is exact.

**Non-finite guard.** If either difference is non-finite the modifier leaves
the hit untouched (`std::isfinite`; the macOS fast-math pairing keeps it
honest, [CLAUDE.md](../CLAUDE.md) §High-Value Facts). No NaN normal ever
reaches a material.

**Frame rebuild** is the shared block verbatim from the ONB-rebuild comment
block in `NormalMap::Modify`
(project the current `u`, `CreateFromWU`, restore handedness with `FlipV`,
`CreateFromW` fallback). It is the fourth copy of that block; Phase 1 hoists
it into a header-inline helper `ModifierFrame::RebuildPreservingTangent(ri,
newN)` and switches the three existing modifiers to it, so the mirrored-
instance handedness fix has one home.

**No geometric-horizon clamp.** Like BumpMap/NormalMap and PBRT's bump
mapping, a large `scale` may push `N'` below the geometric plane; the
materials' geometric-horizon gates handle that (they already expect the two
normals to disagree post-modifier). GlintModifier's rejection is a *discrete*
facet decision and does not transfer.

### 3.3 Step selection — scale-aware and footprint-aware

```
surface: s = max( step_user > 0 ? step_user : 1e-3,  txFootprint.valid ? txFootprint.worldWidth : 0 )
uv:      s = step_user > 0 ? step_user : 0.01
```

Why `max(·, fw)`: a central difference over a span smaller than the pixel
footprint measures sub-pixel slope and sparkles at distance; over a span of
the footprint it measures the footprint-averaged slope, whose magnitude is
bounded by `max|H| / fw` and so *decays* as the footprint grows, which is the
fade we want on fields that have no octave fade of their own (`checker`,
`voronoi`, images). Fields that do fade (the noise builtins, mip-mapped
textures) are already band-limited and the `max` is a no-op on them at the
relevant scales. The `1e-3` floor is a derivative-estimator step in double
precision, not a scene-scale guess; on a scene whose features are below
`1e-3` world units the author sets `step` (the descriptor says so). This is
the one new constant in the design and it is disclosed as such.

**The explicit `step` is a FLOOR, and the fade is mesh-only.** Note what the
`max` does to an author-supplied value: it is raised to the footprint too, so
on geometry that populates one, a `step` below the footprint is silently
ignored. And only **triangle-mesh** geometry populates `txFootprint` today,
and then only on primary hits carrying ray differentials — the same
restriction `fw` in the expression VM already has
([ExpressionPainter.cpp](../src/Library/Painters/ExpressionPainter.cpp) says
so at its `ctx.fw` assignment). On analytic primitives and SDFs the footprint
is unknown (0), the `max` is a no-op, the explicit `step` (or the `1e-3`
floor) is exactly what is used, and there is **no distance fade at all**.
That is the same limitation the whole painter stack carries, not one relief
introduces; the descriptor discloses both halves rather than promising a fade
that only some geometry gets.

**`txFootprint.worldWidth` is now world-correct under instance scale
(2026-09-06, fix round 2, P2-A) — it was NOT before this fix.**
`TextureFootprintCompute::ComputeTextureFootprint` runs mid-
`Object::IntersectRay`, on the ray that function has already transformed
into object space, so `worldWidth` was stamped an OBJECT-space length
despite its name and its doc comment's world-space claim
(RayIntersectionGeometric.h, TextureFootprintCompute.h) — the sibling
length `derivatives.scaleHint` was folded by `m_worldLinearScale` at the
Object layer, but `worldWidth` was not.  Every consumer (`fw` in
ExpressionPainter/ExpressionScalarPainter, and the `max(·, fw)` rule
above) therefore silently read object units on any object with a
non-unit world scale: on a `scale 10` mesh instance the footprint fade
above kicked in ten times later (or never) than the equivalent
unscaled instance, and `receding_pier`-class scenes authored against a
scaled instance faded at the wrong distance.  Fixed by folding
`worldWidth` by `m_worldLinearScale` at the same point `Object::
IntersectRay` / `CSGObject::IntersectRay` fold `scaleHint` (the same
`|det M|^(1/3)` geometric-mean approximation under non-uniform scale,
exact under uniform scale).  Regression: `ReliefModifierTest` test 4c.
On a strongly flattened or elongated instance the geometric-mean fold
can UNDER-scale `worldWidth` relative to the true in-plane footprint
(e.g. `scale 4 0.05 4` on a panel: in-plane scale is 4x but
`|det|^(1/3) = 0.928`, a 4.31x under-count) -- worse than the pre-fold
object-space value would have been on that axis.  The error only
under-filters, though: `max(step, worldWidth)` then falls back to
`step`, the same aliasing as pre-fix and never worse than that floor.
The uniform-scale case remains exact.

### 3.4 One field on the hit record

`RayIntersectionGeometric` gains

```cpp
const Matrix4* pmxWorldToObject;   // linear map for tangent steps; nullptr when unknown
```

defaulted `nullptr`, stamped by `Object::IntersectRay` beside `ri.pModifier`.
`CSGObject::IntersectRay` stamps **`nullptr`** when it overrides the child's
modifier, and every hit on a CSG composite therefore takes the degraded mode
below — one `Warning` per process, object-space height fields stepped in
world units on that object. *(Superseded by Phase 1 — the original wording
here said "with the composite's own inverse", and asked the implementer to
verify which matrix maps the world step onto the `ptObjIntersec` the child
painters read. The answer is **neither**: `AdoptCsgSurfacePayload` copies
`ptObjIntersec` untransformed from the child operand, so the point is in the
CHILD's object space and the map into it is the child's inverse composed with
every enclosing composite's — a per-hit product no member holds and a
`const Matrix4*` cannot express. The composite's inverse is wrong by exactly
the child's transform; the child's is wrong by exactly the composite's. See
§12 deviation 2 for the full reasoning. The consequence is a **known gap**:
an object-space height field on a CSG composite is stepped in world units,
exact only when the whole chain is a pure translation.)* The design's other
half still stands and shipped: a test that a rotated-instance — and a
non-uniformly scaled — object-space height field gives the same `N'` as the
equivalent world-space field (`ReliefModifierTest` test 5). When the pointer is null the
modifier moves `ptObjIntersec` by the world step and says so once in the log
(`Warning`, once per process) — the honest degraded mode for hits produced
by paths that never route through `Object::IntersectRay`. This is an output
field, so `PropagateCastInputs` is unaffected.

### 3.5 Wiring

- `src/Library/Modifiers/ReliefModifier.{h,cpp}` — the class; header-inline
  `ModifierFrame.h` for the shared rebuild. Five build projects per
  [CLAUDE.md](../CLAUDE.md).
- `RISE_API_CreateReliefModifier(IRayIntersectionModifier**, const IScalarPainter& height, Scalar scale, ReliefDomain domain, Scalar step)`.
- `IJob::AddReliefModifier(const char* name, const char* height, double scale, const char* domain, double step)` — a **new** virtual appended at the end of `IJob` (additive ABI, same pattern as `AddGlintModifier`). Resolves `height` through `ResolveOrDiagnoseScalar(..., "relief_modifier", name, "height", value, /*requireSingle*/ true)`.
- `ReliefModifierAsciiChunkParser` in the registry; `cd.category = Modifier`.
- CST: the `height` param is a Reference on the Scalar pipe; the existing
  ScalarPainter closure handles it (no `kFunc2DSubCat` special case — that
  list is for the *legacy* slot and stays until §7.5).

---

## 4. Composition — `modifier_stack`

A single-pointer slot cannot express "normal map, then relief, then glint".
The smallest principled fix is a modifier that is a list:

```
modifier_stack
{
	name      finish
	modifier  nm_paint          # applied first
	modifier  relief_crackle
	modifier  glint_flakes      # applied last
}
```

`modifier` is a repeatable `Reference` on `{Modifier}` (precedent: the
`shaderop` parameter in `StandardShaderAsciiChunkParser::Describe`,
[ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)).
`ModifierStack::Modify` applies them in authored order; each sees the
previous one's `vNormal`/`onb`. Because names resolve at parse time a stack
can contain a stack but never itself. Empty stacks are a parse error (an
authored no-op is a mistake, matching `glint_modifier`'s stance).

**Order semantics, documented in the descriptor and tested:**

| Order | Meaning |
|---|---|
| `normal_map` → `relief` | relief perturbs the *normal-mapped* frame: fine procedural detail on top of a baked map (the usual case). |
| `relief` → `normal_map` | the map is decoded in the *relief-tilted* frame; rarely wanted. |
| `… → glint` | glint always last: it replaces the normal with a facet normal drawn about the current one, so it must see the final smooth frame. |

`relief` twice with different fields is legitimate (two-frequency relief)
and is how a "coarse + fine" split is expressed when both are shading-only.

The composite-override rule in `CSGObject` (composite's modifier replaces the
child's) is unchanged; an author who wants both puts both in a stack on the
composite.

---

## 5. How the perturbed normal flows — audit

### 5.1 Materials

Every SPF/BRDF shades in `ri.onb` (`w()` = the perturbed normal) and gates
sidedness against `ri.vGeomNormal`; the audit in §2 found no material that
caches a pre-modifier frame. Specifically:

- **GGX / Cook-Torrance / coated / dielectric / Lambertian** — read `onb.w()`
  (and `u()/v()` for anisotropy). Relief changes the frame, the geometric
  gate stays put. Nothing to change.
- **hair_material** — `onb.u()` is the fiber tangent (`bHasShadingTangent`).
  The shared rebuild projects the *current* `u` onto the new plane, so relief
  on a hair hit keeps the fiber axis to first order. Relief on hair is
  pointless (a fiber has no surface to emboss) and the recipe says not to;
  the guarantee is only that it does not break the frame.
- **fabric / weave** — read `onb.w()` only at the SPF layer; weave direction
  comes from `weave_rotation` painters, not the frame. Relief composes.
- **BSSRDF / SSS** — the front-face gate uses the geometric normal and the
  Fresnel cosine the shading normal (the "Front-face gate uses the GEOMETRIC
  normal" comment in `PathTracingIntegrator::IntegrateFromHitTemplated`,
  [PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp)); unchanged.

### 5.2 Integrators

PT, BDPT, VCM, MLT, the photon tracers, SMS and SSS all call `Modify` once
per hit before any material call and cache `geomNormal` separately (§2). A
new modifier is invisible to them by construction. Verification is
therefore a **parity check**, not new integrator code: the relief test scene
(§8) rendered under `pathtracing_pel_rasterizer` and `bdpt_pel_rasterizer`
must agree in mean luminance within the tolerance the existing PT-vs-BDPT
suites use — that is the regression guard that the per-vertex caching keeps
carrying the modified frame.

### 5.3 With `displaced_geometry`

Displacement moves vertices; relief tilts normals. The recommended split:

- **Coarse shape** (silhouette-changing, self-shadowing) → `displaced_geometry`
  with a UV `expression_function2d` at `disp_scale`.
- **Fine relief** (sub-silhouette; pores, grain, crackle) → `relief_modifier`
  on the *displaced object*, height from the fine field.

Sharing one field between them is possible today **only for UV fields**
(§1.1 item 1): bind the same `expression_function2d` to `displacement` and to
`scalar_painter { function2d F }` → `relief_modifier { domain uv }`. For 3D
fields the two stay separate until `displaced_geometry` learns the scalar
pipe (chipped follow-up). The recipe (§9) shows the UV-shared form and says
why.

`displaced_geometry`'s baked mesh sets neither `bHasTangent` nor
`bHasShadingTangent`, so relief on it takes the `CreateFromW` path — fine in
`surface` mode (§3.2), arbitrary-orientation in `uv` mode exactly as
`bumpmap_modifier` is today.

---

## 6. Alternatives considered

- **Extend `bumpmap_modifier` with a 3D mode.** Keeps a chunk whose name,
  `function` slot (Function2D-piped, painter-dual-registered, R-channel,
  UV-clamped) and inverted sign convention are all wrong for the scalar
  pipe. Migration would still be needed for the sign. Rejected.
- **Analytic gradient through the expression VM.** A forward-mode dual
  number VM would give exact `∇H` in one evaluation instead of four. It is
  the right long-term answer for expression fields only; images, voronoi,
  ramps and channel bridges have no analytic gradient. Central difference is
  uniform across every painter kind and is what PBRT does. Deferred; noted
  as the refinement if the 4× cost ever shows in a profile (§10).
- **Relief as a material property (`bump` slot on every material).** The
  modifier hook already fires before every material call in every transport
  path; a per-material slot would need N material edits and would not
  compose with normal maps. Rejected.
- **Auto-wrap an `IPainter` bound to `height`.** Convenient, but it silently
  picks a channel and defeats the routing-by-physical-meaning rule; the
  standing diagnostic plus a one-line wrap is the established idiom.
  Rejected.
- **Geometric-horizon clamp inside the modifier.** Would create flat spots at
  the clamp boundary; the material gates already handle it continuously.
  Rejected (§3.2).

---

## 7. Deprecating `bumpmap_modifier`

### 7.1 Phase A — now (this arc): deprecate with diagnostic, keep parsing

Following precedent (c) plus a parse-time diagnostic:

- `BumpmapModifierAsciiChunkParser::Finalize` emits, once per process,
  `eLog_Warning`: *"bumpmap_modifier `%s` is DEPRECATED and will be removed
  in a later release: use relief_modifier (any scalar_painter height field,
  no texcoords required). Migrate this scene losslessly with
  tools/migrate_scenes_relief.py."* Once per process, not per chunk —
  `NormalMap.cpp`'s `std::atomic<bool>` idiom.
- The descriptor `description` is prefixed `"DEPRECATED — use relief_modifier
  (…)"`, which propagates to the agent tool schema and the GUI suggestion
  surfaces (§2), so no author, human or agent, is offered it as a first
  choice again.
- `IJob::AddBumpMapModifier`, `RISE_API_CreateBumpMapModifier{,Ex}`, and the
  `BumpMap` class are untouched (ABI freeze; the Blender bridge still
  compiles and works).
- Every in-tree scene is migrated (§7.3); the CST tests that embed
  `bumpmap_modifier` literals keep passing under the warning and gain
  `relief_modifier` twins so the closure logic is proven on the new slot.

### 7.2 The migrator — `tools/migrate_scenes_relief.py`

Line-oriented, comment-stripped structural pass (the
`migrate_scenes_light_colorspace.py` lexer-mirroring idiom), `--root`,
`--dry-run`, `--selftest`, idempotent (a file with no `bumpmap_modifier`
chunk is untouched; re-running on migrated output is a no-op). For each

```
bumpmap_modifier { name N  function F  scale S  windowsize W  normalize_gradient G }
```

it emits, in place,

```
scalar_painter   { name N__height  function2d F }
relief_modifier  { name N  height N__height  domain uv  step W  scale S' }
```

with `S' = −S` when `G` is TRUE and `S' = −S·2W` when FALSE (defaults `S=1`,
`W=0.01`, `G=FALSE` applied when absent). Derivation: legacy tilt is
`+T·S·(f₊−f₋)` (or `/2W` when normalized); the new modifier tilts
`−T·S'·(f₊−f₋)/(2W)`; equate. `Function2DScalarPainter` calls the same
`F.Evaluate(ptCoord)` the legacy modifier does, so the sampled values are
identical (same fake-hit path, same UV clamp, same R channel); the only
difference is FP reassociation of the `scale` multiply, which the unit test
bounds at `1e-12`. The object's `modifier N` line needs no edit. `S'` is
printed with Python's `repr(float(x))` **unconditionally** (David Gay's
shortest round-trip decimal — what turns `-(0.5*2*0.005)` into `-0.005`, not
`-0.0050000000000000001`); it is not conditioned on a digit-count threshold,
and `%.17g` is not used anywhere in this script.

**`windowsize ≤ 0` is not a point on that curve — it is a special case.**
Legacy `BumpMap::Modify` is INERT there (the central difference samples the
same point on both sides and its normalisation is gated on `dWindow > 0`),
but a migrated `step 0` means AUTO in `relief_modifier` (a full
footprint/`1e-3`-floor perturbation) — the opposite of inert. The migrator
detects `windowsize ≤ 0`, skips the algebra above, and emits the bare token
`scale 0` instead (which neutralises the perturbation regardless of what
`step` ends up being), with a `WARN <file>:<line>` naming the reason.

Migration is *lossless*, not *improving*: migrated scenes keep the UV
domain and the legacy orientation behaviour. The migrator prints a per-file
note naming `domain surface` as the upgrade, and §9's recipe shows it.

### 7.3 In-tree corpus

| Scene | Action |
|---|---|
| `scenes/FeatureBased/Materials/velvet_cushion.RISEscene` | migrate (normalized; `S' = −0.0075`) |
| `scenes/FeatureBased/Combined/sculptors_studio.RISEscene` | migrate (`S' = −0.5·2·0.005 = −0.005`) |
| `scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene` | migrate (`S' = −10·2·0.001 = −0.02`); the SMS results docs that cite this scene's bump are historical records and keep their text |
| `scenes/Internal/pool.RISEscene` | migrate on disk (gitignored; not in the golden) |
| `tests/Cst{Resolver,RecordDerive,IncrementalSafety,SourceInstance}Test.cpp` | keep the legacy literals (they test the deprecated path) and add `relief_modifier` twins |
| `tests/data/cst_derive_golden.txt` | regenerate **after `git add`** of every new scene (the ls-files lesson); expected diff is **additions-only** for the new scenes — the migrated entries' own digests do **not** change, because `DumpJob` (`tests/CstRenderEquivalence.h`) records an object's modifier binding by NAME only (`modifier=<name>`), never the bound modifier's concrete type or parameters, and the migrator does not touch the object's `modifier N` line (§7.2); zero drift elsewhere. See §12's Phase 3 record for the actual regen run and why "no digest change on the 3 migrated scenes" is the correct result, not a gap. |

### 7.4 What else consumes `expression_function2d` afterwards

`displaced_geometry.displacement` (10 scenes), `function2d_painter` (4),
`scalar_painter { function2d }` (1, `watch_dial`), `composite_function2d_painter`,
`sdf_geometry.heightfield_function`. The frozen UV-only contract stays
frozen (the "`expression_function2d` stays frozen" item in
[GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)).
Its teaching line — the `expression_function2d` row of the pattern-selection
table in `procedural-textures.md` — is rewritten to name
**displacement** as its niche and to stop implying any normal-perturbation
use. The legacy surface shrinks by one consumer; the remaining ones are all
vertex-time or explicit-bridge uses, which is the right shape for a frozen
evaluator.

**Post-migration recount (Phase 3, a scene-parsing script over every
`scenes/**/*.RISEscene`, matching each `displaced_geometry` /
`function2d_painter` / `scalar_painter` / `composite_function2d_painter` /
`sdf_geometry` binding against the `expression_function2d` names actually
declared in the SAME file — not a bare grep for the string, which
over-counts chunk declarations that merely mention the keyword):
`displaced_geometry.displacement` **5** scenes (the pre-migration "10"
above was never re-verified against source; this count IS, by the same
script, against the current tree — `dreamscape_coral_queens_hour{,_v2}`,
`dreamscape_covenant_of_the_vale`, `watch_dial`, `misc_geometry_stress`),
`function2d_painter` **4** (`denim_and_satin_drape`, `fabric_swatches`,
`sheer_curtain`, `cc_function2d_painter` — unchanged), `scalar_painter
{ function2d }` **2** (`watch_dial` plus, new this phase, `velvet_cushion` —
the migrator's output), `composite_function2d_painter` **0** direct
`expression_function2d` children in the current corpus,
`sdf_geometry.heightfield_function` **5** (`enamel_watch`, `watch_dial`,
`enamel_depth_glints`, `enamel_dial_dimpled`, `enamel_dial_sdf`).
**Zero** `bumpmap_modifier` chunks remain anywhere under `scenes/`
(`grep -rl '^bumpmap_modifier' scenes/` — empty), confirming the migration
in §7.3 is total. `sculptors_studio`, `sms_veach_egg_bumpmap` and
`Internal/pool` do NOT appear in the `expression_function2d` consumer list
at all, migrated or not — their legacy bump functions were `perlin2d_painter`
/ `checker_painter` / `png_painter`, not `expression_function2d`; only
`velvet_cushion`'s `crease_field` was. **The "shrinking legacy surface"
statement is confirmed, not just repeated**: before this arc, SIX chunk
kinds could bind an `expression_function2d` name (the five enumerated
above — `displaced_geometry.displacement`, `function2d_painter`,
`scalar_painter { function2d }`, `composite_function2d_painter`,
`sdf_geometry.heightfield_function` — plus `bumpmap_modifier.function`);
after Phase 3, exactly FIVE can, all vertex-time or explicit-bridge uses
— `bumpmap_modifier` still parses (it is Phase A, not removed) but zero
in-tree instances exercise that sixth path any more, and the deprecation
diagnostic (§7.1) means a new one is unlikely to appear without a warning
pointing the author elsewhere.

### 7.5 Phase B — removal (named, not in this arc)

Trigger: the user's call after one release cycle (or after the census in
§9 shows zero `bumpmap_modifier` emissions). Steps: delete the chunk parser
(unknown-chunk hard fail is the generic path, like `branching_threshold`),
delete `BumpMap.{h,cpp}` from the five build projects, repoint the frozen
`IJob::AddBumpMapModifier` virtual at `ReliefModifier` in UV domain with the
§7.2 scale fold (the Blender bridge keeps working unchanged), drop the
`kFunc2DSubCat` special case for `bumpmap_modifier.function` in `Cst.cpp`,
retire the legacy CST literals. Everything Phase A leaves in place is
enumerated here so Phase B is a checklist, not a rediscovery.

---

## 8. Tests

`tests/ReliefModifierTest.cpp` (unit; builds a hit like `GlintModifierTest`):

1. **Analytic gradient.** Height `expression P.x`, `scale s`, `N=(0,0,1)`,
   `T=(1,0,0)`: `N' = normalize(0,0,1) − s·(1,0,0)` exactly (within `1e-12`);
   sign proves the Blinn convention.
2. **Legacy equivalence.** For a `perlin2d_painter` and both
   `normalize_gradient` values, `BumpMap(F, S, W, G)` and
   `Relief(Function2DScalarPainter(F), S', uv, W)` agree on `vNormal` and
   `onb` within `1e-12` at 1000 random `(u,v)` — the migrator's algebra,
   red-proven by flipping the sign.
3. **No texcoords.** `ptCoord` untouched, `derivatives.valid = false`, a 3D
   field still perturbs; and `ptCoord` is unchanged after `Modify`.
4. **Footprint fade.** With `txFootprint.valid`, `|N'−N|` is non-increasing
   in `worldWidth` over a decade sweep on an `fbm` height (the octave fade
   plus the `max(·, fw)` step rule).
5. **Object-space exactness.** A `mapping_painter space object` height on a
   rotated instance equals the world-space equivalent field's `N'` (needs
   `pmxWorldToObject`); red-proof by nulling the pointer.
6. **UV chain rule.** A UV-domain painter in `surface` mode with valid
   `dpdu/dpdv` matches `domain uv` on a plane whose UV is the identity map.
7. **Handedness.** Mirrored incoming frame (`FlipV`) is preserved after
   rebuild; mirrors `NormalMap`'s guarantee.
8. **Non-finite guard.** A height that returns NaN leaves the hit untouched.
9. **Stack order.** `modifier_stack` applies in order; `normal_map → relief`
   and `relief → normal_map` differ; empty stack rejected at parse.
10. **Parse.** `relief_modifier` round-trips; `height` bound to an `IPainter`
    yields `kScalarBoundToIPainterFmt`; unknown name yields
    `kScalarUnknownFmt`; the `bumpmap_modifier` deprecation warning fires
    once for two chunks.

Scenes: `scenes/Tests/ChunkCoverage/cc_relief_modifier.RISEscene`,
`cc_modifier_stack.RISEscene`; `scenes/Tests/Painters/relief_sphere_no_uv.RISEscene`
(analytic sphere, 3D expression height; the PT-vs-BDPT parity fixture of
§5.2). Migrator: `--selftest` covers idempotency, defaults, both signs, a
brace hidden behind a comment.

Gate per phase: the touched suites only (`ReliefModifierTest`,
`GlintModifierTest`, the four CST tests, `CstDeriveGoldenTest`,
`ScalarPainterParserTest`, `AgentChunkCrudTest` when the advisory lands),
never the full suite ([memory: minimal build/test cadence]). Warning-free
clean rebuild of the touched objects on `make`; the Xcode gate at the arc's
end.

---

## 9. Adoption wiring

Per doc 88 §2 the measured laws are: advice ≈ 0 (C-ADV), the failure is a
typing prior (C-TYPE), when advice fails ship a verb (C-VERB), the summoned
read-set is `object-modeling-recipes.md` + `materials-and-media-basics.md`
(C-READ), one execution-validated parsing example per mechanism, and no
tolls (§7 decision 2, reaffirmed in the "Price the inferior path" row of
[WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md)'s adoption-wiring table).

- **Recipe.** One worked example in `materials-and-media-basics.md`: a
  crackle-glaze ceramic where **one** `expression_painter` cell field drives
  colour (ramp), roughness (`scalar_painter { painter … channel R }`) and
  relief (`relief_modifier` on the same bridge, negative scale so cracks
  sink). Execution-validated (it is the §10 fixture). `procedural-textures.md`
  gets the reference row and the hook-line rewrite; everything else gets a
  pointer.
- **Advisory `DESIGN_FLAT_RELIEF`** (the decal-on-plastic detector): fires
  when an object's **effective** material has a **genuinely**
  spatially-varying painter on a non-emission colour slot *and* the
  rendered surface carries **no effective modifier**; names the object,
  the field, and the two-chunk fix. Advisory-only. Its adoption is
  expected to be ≈ 0 per C-ADV; it exists to make the census (below)
  measurable, and its text is the one place the pattern is taught at the
  moment the agent is looking at the material.

  All three emphasised words are load-bearing, and the two fix rounds in
  §12 are why (the first cut of this condition got each of them wrong):

  - **Effective modifier**, not "the object chunk spells `modifier`". The
    engine binds one by four routes, and this condition silences on any of
    them: the object's own, one **inherited down a `source` chain**
    (`MergeChunkParams` folds the chain in and `modifier` is not
    instance-own, so an instancing copy renders with its source's relief —
    unless the copy spells `modifier none`, which genuinely clears it),
    **every operand of a `csg_object`** carrying one (a hit reports the
    operand's modifier, so a composite binding nothing is still
    relief-bearing when both operands are), and **an enclosing composite**
    binding one over an operand (the composite's binding takes final
    precedence). A composite with no modifier and only *some* operands
    bearing relief still fires, naming the composite. Any modifier KIND
    counts — a `modifier_stack`, a bumpmap, a normal map — since the claim
    is narrowly "the shading normal is inert here".
  - **Effective material**, not "the object chunk spells `material`" —
    the same two engine rules, because both govern the *pair*
    `pMaterial`/`pModifier` and not the modifier alone
    (`AdoptCsgSurfaceBindings` copies both, `IsInstanceOwnParam` excludes
    both, and the composite applies its own through a matched
    `if( pMaterial ) … if( pModifier ) …` pair). So a `copy { source
    orig }` that spells no material of its own is judged on the material
    it **inherits** (and `material none` clears it); and an operand under
    a composite that spells a `material` is **not** a candidate on its
    own material — that texture is never shaded, so the composite is the
    candidate, on the material it spells. Nested: any enclosing composite
    spelling one silences the operand. Silencing an operand is safe
    because **a CSG operand never renders standalone** —
    `CSGObject::AssignObjects` consumes both operands (`IsWorldVisible()`
    is `bIsWorldVisible && nConsumedBy == 0`) and `Job::AddCSGObject`
    refuses a `parent`ed operand, so the only surface an operand
    contributes to is its composite's.
  - **Genuinely varying**, not "the painter kind is not one of the three
    constant ones". A `blend_painter` / `ramp_painter` / `mapping_painter`
    / `channel_painter` whose inputs are all uniform is structurally
    constant and does not qualify; a colour slot `add_wetness` rebound to
    its own wetness-prelude expression is a wet film, not authored
    texture, and does not qualify either (advising relief there is
    physically backwards — a film conforms to relief rather than adding
    it). Conversely, a varying slot on a **wrapped base** — under a
    `coated_material`, `fabric_material` or `composite_material` — *does*
    qualify: the advice is about the surface under the wrapper.
  - **Non-emission slot.** A varying `emissive` over flat `rd`/`rs` is a
    painted glow, a complete look on its own; relief cannot sell it. The
    exclusion is slot-scoped, so the same material with a varying `rd`
    still fires.
- **Verb hook points** (assessed, not shipped): `add_wear` already mints a
  `curv`-driven `expression_painter` and rebinds colour and roughness
  (`AgentSession::AddWear` in `AgentSession.cpp`); relief cannot come from `curv` (§3.2 —
  signals are locally constant), so the natural hook is an optional
  `relief_amplitude` argument that mints the wear *noise* term as a
  `scalar_painter` and a `relief_modifier` on the target object, wrapping
  any existing modifier in a `modifier_stack`. `add_wetness` should **not**
  add relief (a wet film smooths). Per C-VERB, if the census shows the
  advisory + recipe do not move adoption, the verb argument is the named
  escalation; it is not built until then.
- **Census.** User-run, as every prior arc (baseline at the branch point,
  post-arc at merge): count `relief_modifier` chunks per trajectory and
  `DESIGN_FLAT_RELIEF` firings; N ≥ 3, cross-provider before believing a
  null (C-MEAS).

---

## 10. Cost

*(rewritten against the Phase-1 measurements; the pre-implementation draft
asserted "~4× one albedo evaluation" as though measured, which it was not —
see the §12 cost table for what actually was.)*

**The count is exact and is the honest claim.** Four extra height evaluations
per hit on objects that bind relief; **zero** on objects that do not.

**Measured, on the §8 fixture at 256×256 / 64 spp, 7 runs per variant:**

- The modifier's own machinery — four `RayIntersectionGeometric` copies, the
  point / object-space / UV offsets, the frame rebuild — costs **+3.9 %**
  whole-render (a relief with a *constant* height vs. no modifier at all).
  That is not where the money goes.
- Whole-render, relief on vs. off with an `fbm` height is **3.39×**. That is
  a **near-worst case by construction**: the fixture is one sphere whose only
  expensive work *is* the height field, so nothing dilutes the four
  evaluations.
- Isolating the field: relief's four evaluations cost **1.73×** what the same
  field costs bound to the albedo slot. This figure **does not confirm a "4×
  one albedo evaluation" reading and must not be quoted as one** — it is
  smaller, and the comparison is not apples-to-apples, because the albedo slot
  is evidently queried more than once per hit on this Lambertian + NEE path,
  so the albedo delta is not "one evaluation".

The dual-number VM (§6) is the named refinement if it matters; it does not for
the showcase fixtures at their authored spp.

---

## 11. Phases

| Phase | Deliverable | Gate |
|---|---|---|
| **1** | `ReliefModifier` + `ModifierFrame.h` hoist + `pmxWorldToObject` + API/IJob/parser + 5 build projects + `ReliefModifierTest` 1–8, 10 + `cc_relief_modifier` + `relief_sphere_no_uv` | zero-P1 round; PT/BDPT parity on the sphere |
| **2** | `modifier_stack` + test 9 + `cc_modifier_stack` + §4 order doc in the descriptor | reviewed: one adversarial round, 0 correctness P1s (R9 CLEAN incl. `leaks --atExit` on nested stacks), 2 citation P1s fixed here (see §12) |
| **3** | deprecation diagnostic + migrator + migrate 4 scenes + CST twins + golden regen + teaching surfaces (skills, `Parsers/README.md`, `GLTF_IMPORT.md` living text, `MATERIALS.md`, descriptor text) + §7.4 audit note | Phase 3 reviewed (three lenses: code CLEAN after 1 P1 fix, teaching 3 P1s fixed, migration LOSSLESS by pixels — see §12): golden additions-only beyond migrated entries (the 1 pre-existing DRIFT is `bdpt_crystal_garden`, out of scope) |
| **4** | `DESIGN_FLAT_RELIEF` (condition Q) + verb hook-point notes (the recipe example shipped early, as part of Phase 3's teaching surfaces — `materials-and-media-basics.md`'s crackle-glaze — see its own §12 P1-1 fix) | implemented 2026-09-06, two review rounds (see §12 — round 1 `177bcee4`: 3 P1s + 3 P2s, the scan made linear; round 2 `e5d0fa0b`: 1 P1 + 2 P2s + a nit, clause (i) taught the same composite-override and `source`-inheritance rules round 1 taught clause (ii), and the `source` hop bound raised to the engine's own 256 across all three walks in the file): `AgentReadValidateTest` 333/0 (extended with `RunFlatReliefScanTest`), `AgentChunkCrudTest` 3809/0, `AgentAddWearTest` 287/0, `AgentAddWetnessTest` 210/0, `SourceHygieneTest` 164/0, `ReliefModifierTest` 106/0 |
| **5** | pixel verification: `weathered_workbench` before/after with relief bound to `expr_grain` (kept in the showcase), `velvet_cushion` migrated vs. `domain surface` upgrade; look, and record | landed 2026-09-06 (see §12): `weathered_workbench` relief committed (scale 0.05, amplitude-swept); `velvet_cushion` migration confirmed better (domain surface reads flat on this SDF, no code change); `relief_crackle_glaze` key light re-raked for near-specular legibility (R13's finding, one commit); renders attached to §12 |

Commits as each phase converges; never push. The stray uncommitted edit to
`scenes/FeatureBased/BDPT/bdpt_crystal_garden.RISEscene` predates this arc
and is left alone.

---

## 12. Phase record

*(appended as phases land)*

### Phase 1 — landed 2026-09-05

Branch `relief-modifier`, **eight** commits off `563204b8` (the table below;
`git rev-list --count 563204b8..27aef6ac` = 8).  A ninth, `c3600ed0`, landed
separately on the same branch after this record was written: it is
`tools/migrate_scenes_relief.py`, the **Phase-3** migrator (lossless
`bumpmap_modifier` → `scalar_painter{function2d}` + `relief_modifier{domain
uv}`, the §7.2 scale fold, `--dry-run`/`--selftest` with 12 selftest cases).
It is listed here only so the branch's commit range is accounted for; it is
NOT a Phase-1 deliverable and its phase gate is Phase 3's.

| Commit | What |
|---|---|
| `9b4f29fb` | `ModifierFrame.h` hoist + `pmxWorldToObject` on the hit record |
| `3deff3e2` | `ReliefModifier.{h,cpp}` + API/IJob/Job/parser + all five build projects |
| `621b5aae` | `tests/ReliefModifierTest.cpp` (tests 1–8, 10) |
| `547c417d` | `cc_relief_modifier` + `relief_sphere_no_uv` |
| `0ea40138` | hygiene opt-out for the two non-finite test fixtures |
| `25db73b6` | test 5 gains the non-uniform-scale case |
| `d2c7b29f` | this record |

**Files.** New: `src/Library/Modifiers/ModifierFrame.h`,
`src/Library/Modifiers/ReliefModifier.{h,cpp}`,
`tests/ReliefModifierTest.cpp`,
`scenes/Tests/ChunkCoverage/cc_relief_modifier.RISEscene`,
`scenes/Tests/Painters/relief_sphere_no_uv.RISEscene`.  Modified:
`BumpMap.cpp`, `NormalMap.cpp`, `GlintModifier.cpp`,
`RayIntersectionGeometric.h`, `Object.cpp`, `CSGObject.cpp`,
`RISE_API.{h,cpp}`, `IJob.h`, `Job.{h,cpp}`, `ChunkParserRegistry.cpp`,
`Parsers/README.md`, `tests/IJobVtableManifest.txt`, and the five build
projects.

**Tests.** `ReliefModifierTest` **62 checks, 0 failures** (59 at commit `621b5aae`; `25db73b6` added the non-uniform-scale case to test 5, since a rotation preserves lengths and therefore could not discriminate the pmxWorldToObject doc comment's "the step goes through the LINEAR map" claim from the null-pointer degraded mode).  Gate suites, all
run on the final tree: `GlintModifierTest` ALL PASSED,
`HairTangentPlumbingTest` 123/0, `SurfaceCurvatureTest` 94/0,
`CstResolverTest` 44/0, `CstRecordDeriveTest` 23/0,
`CstIncrementalSafetyTest` 38/0, `CstSourceInstanceTest` 455/0,
`ScalarPainterParserTest` 60/0, `SourceHygieneTest` 164/0,
`BDPTVertexRIGRebuildTest` 15/0.  **`SceneEditorSuggestionsTest` belongs on
this list and was missing from it** — the suite hard-codes the registered-chunk
count in two EXPECTs, so *any* new chunk turns it red until they are bumped;
`relief_modifier` did, and fix round 1 (P1-C) bumped 174 → 175.  It is a
**Phase-2 gate too**: `modifier_stack` will make it 176.  `ObjectMirrorTest` is 161 passed / **1
failed** — `"B: front/back face classification survives the reflection"`
— and that failure is **PRE-EXISTING**, verified by building the same
test from a pristine `git archive` of `563204b8` in a scratch directory
and observing the identical 161/1 with the identical assertion.  It is
unrelated to this arc (a back-face probe with no modifier in play).

**Red-proofs.** (a) flipping the perturbation sign to `N + …` → 14
failures including all 4000 of test 2's legacy comparisons; (b) nulling
`pmxWorldToObject` → asserted live inside test 5; (c) the finiteness
guards → **measured mutually redundant**: removing either alone leaves
the suite green, only removing both fails test 8 (6 failures, real
NaN/Inf normals reaching the frame rebuild — which also proves the
literals are not folded on this build); (d) dropping the `ptCoord` chain
rule → test 6 fails, 200/200 mismatches.  Every mutation was reverted.

**PT-vs-BDPT parity (§5.2).**  `relief_sphere_no_uv` at 256×256, 16 spp,
`oidn_denoise FALSE`, mean linear Rec.709 luminance over all 65 536
pixels: PT `0.239393`, BDPT `0.239416` — **ratio 1.0001, 0.01 % apart**,
against the 8 % mean band `BDPTStrategyBalanceTest` uses.  BDPT's
per-vertex `BDPTVertex` caching is carrying the modified frame.

**Cost — measured, not asserted.**  Four variants of the fixture at
256×256, **64 spp**, 7 runs each, "Total Rasterization Time":

| Variant | mean | σ | Δ vs A |
|---|---|---|---|
| A — no modifier, uniform albedo | 348.9 ms | 5.9 | — |
| D — relief with a **constant** height | 362.6 ms | 11.1 | +13.7 ms (**+3.9 %**) |
| B — no modifier, the same `fbm` as the **albedo** | 823.6 ms | 5.8 | +474.7 ms |
| C — relief with the `fbm` height | 1184.0 ms | 9.8 | +835.1 ms |

Read: the modifier's own machinery — four `RayIntersectionGeometric`
copies, the point/object/UV offsets, the frame rebuild — is **+3.9 %**
(A→D) and is not where the money goes.  Whole-render, relief on vs off
on this fixture is **3.39×** (A→C); that is a near-worst case by
construction, since the fixture is one sphere whose only expensive work
IS the height field.  Isolating the field: relief's four evaluations
cost **1.73×** what the same field costs bound to the albedo slot
(`(C−D)/(B−A)`).

That last figure **does not confirm the pre-implementation §10 draft's
"~4× one albedo evaluation"** (§10 has since been rewritten against this
table, fix round 1 P1-B);
it is smaller, and the honest reading is that the comparison is not
apples-to-apples — the albedo slot is evidently queried more than once
per hit on this Lambertian + NEE path, so `B−A` is not "one evaluation".
What is directly verifiable is the count: the modifier performs **exactly
four** height evaluations per hit on an object that binds it, and zero on
objects that do not.  §10 now states that count, and the three measured
figures, instead of the unmeasured 4× ratio.

**Deviations from the design, with reasons.**

1. **`ReliefDomain` is an `enum class`**, not the plain `enum` §3.4's
   sketch writes.  Scoped, matching `OidnQuality`/`OidnDevice`; no
   implicit int conversion into the API's `Scalar` parameters next to it.
2. **`CSGObject::IntersectRay` stamps `nullptr`, not "the composite's own
   inverse"** as §3.4 parenthetically suggested.  §3.4 asked the
   implementer to verify which matrix maps the world step onto the
   `ptObjIntersec` the child painters read; the answer is **neither**.
   `AdoptCsgSurfacePayload` copies `ptObjIntersec` **untransformed** from
   the child operand (CSGObject.cpp ~325-329, whose own comment names the
   resulting frame mismatch as a deliberate pre-existing gap), so that
   point is in the CHILD's object space and the map into it is the
   child's inverse composed with every enclosing composite's — a per-hit
   product no member holds and a `const Matrix4*` cannot express.  The
   composite's inverse is wrong by exactly the child's transform; the
   child's is wrong by exactly the composite's.  `nullptr` plus the
   documented degraded mode (move by the world step, warn once) is the
   honest third answer, and is still exact whenever the chain is a pure
   translation.  **A CSG object with an object-space relief height is
   therefore a known gap**, not a silent one.
3. **`ModifierFrame::RebuildPreservingTangent` is the shared BODY, not
   the shared policy.**  §3.2 said "verbatim from NormalMap.cpp:220-234";
   NormalMap and BumpMap gate the projection while GlintModifier runs it
   unconditionally, and the two are **observably different** — for a hit
   with no coherent tangent the gated form rebuilds with `CreateFromW`
   (an arbitrary canonical-axis `u`) and the unconditional form projects
   the previous `u`, giving different `u`/`v` about the same `w`.  Folding
   either into the helper would have changed one of the three modifiers'
   behaviour, so the gate stayed at each call site and all three are
   byte-identical to before.  ReliefModifier follows the BumpMap/NormalMap
   gate (it is a height-gradient tilt, the same family).  **⚠ The gate's
   CONDITION was wrong as shipped in Phase 1 — `ri.bHasShadingTangent`
   alone, which misses SDFGeometry's heightfield mode; corrected in fix
   round 1, P1-A below, to `ModifierFrame::HasCoherentTangent`.  The
   body/policy split described here is unchanged by that fix.**  Verified
   green:
   `GlintModifierTest` (incl. its test 5 "tangent direction preserved
   against a NON-canonical base tangent" and test 11 handedness) and
   `HairTangentPlumbingTest`'s four bump/normal-map cases including
   "non-hair hit byte-matches legacy CreateFromW rebuild".
4. **One guard the design did not name**: a `mag2 > 1e-12 && isfinite`
   bail on the perturbed vector.  §3.2's "no NaN normal ever reaches a
   material" implies it; without it `Normalize` could receive an unusable
   vector.  It is a bail, not a clamp — no flat spot, and still no
   geometric-horizon clamp.  *(Fix round 1, P2-2: this entry originally
   said the guard was "for a gradient large enough to cancel `N`", which
   is impossible — `T` and `B` are orthonormal to `N`, so
   `|N − scale·(T·h_T + B·h_B)|² = 1 + scale²(h_T² + h_B²) ≥ 1` for any
   finite gradient on a unit `N`.  What it can actually catch is a
   non-finite `mag2` — redundant with the `isfinite` gate above it, which
   red-proof (c) measured — and a zero-length incoming `N` from a singular
   transform, which nothing upstream covers.  The code comment now says
   that.)*
5. **No `Cst.cpp` change.**  §3.5 anticipated none and that is confirmed:
   `FunctionSubNamespace` maps `bumpmap_modifier`'s `function` to
   `kFunc2DSubCat` because the engine binds it through `pFunc2DManager`,
   but `height` is bound through the **scalar painter** manager, so it
   correctly resolves coarsely against `{Painter}` where the existing
   colour/scalar alias handles the two managers.

**Left undone, deliberately.**  `tests/data/cst_derive_golden.txt` is NOT
regenerated, so `CstDeriveGoldenTest` reports the two new scenes as
`UNCOVERED`.  §11 puts the regen in Phase 3, after the four migrated
scenes land, in one reviewed pass — a partial regen here would make that
diff unreadable.

---

### Phase 1 — fix round 1 (2026-09-06)

Three independent reviewers on `9b4f29fb..27aef6ac` returned **4 P1s and 6
P2s**.  All ten are fixed.  Suite after the round: `ReliefModifierTest`
**80/0** (was 62/0 — test 7b and test 4's second sweep are the additions).

| Finding | Fix | Commit |
|---|---|---|
| **P1-A** — the frame-rebuild gate uses the wrong flag.  `Object::IntersectRay` builds the coherent `CreateFromWU` frame under `bShadingTangentFromGeometry`; `bHasShadingTangent` is only the sub-case inside it.  ReliefModifier, **BumpMap and NormalMap** all gated on the sub-case, so an SDFGeometry-heightfield hit (`bShadingTangentFromGeometry` without `bHasShadingTangent`, SDFGeometry.cpp's `m_isHeightfield` branch of `IntersectRay`) fell to `CreateFromW` — a 180° frame rotation (u:+X→−X, v:+Y→−Y) plus loss of the mirrored-instance `FlipV`. | Root fix, once: `ModifierFrame::HasCoherentTangent( ri )` = `bShadingTangentFromGeometry \|\| bHasShadingTangent`, with a comment citing Object.cpp:699 and justifying the OR (the second disjunct is unreachable in tree; if a future geometry took it, projecting the incoming `u` is harmless — Glint's own continuity argument — so the OR can only ADD preservation).  All three modifiers switched.  Glint stays unconditional, and `ModifierFrame.h` now states that both policies are correct on a coherent-tangent hit and differ only on a tangent-less one.  The header's false claim that "SDFGeometry's heightfield mode" was covered by the `bHasShadingTangent` gate is corrected. | `44cf535d` |
| **P1-B** — design body contradicted §12.  §3.4 still said CSGObject stamps "the composite's own inverse" (it stamps `nullptr`); §10 still asserted a "measured ~4× one albedo evaluation" (never measured); §12 said "four commits" (eight); §2's hook-timing row named `bHasShadingTangent` as the ONB branch condition — the same error that produced P1-A. | §3.4 rewritten to the shipped behaviour with the original wording marked superseded; §10 rewritten against §12's table (exact count of four evaluations; +3.9 % machinery; 3.39× whole-render near-worst-case; the 1.73× vs-albedo figure explicitly flagged as not apples-to-apples); §12's commit count corrected and `c3600ed0` accounted for as the separately-landed **Phase-3** migrator; §2's two rows corrected.  Also swept the enumeration family: the `vNormal` and `vGeomNormal` field comments in `RayIntersectionGeometric.h`, `SurfaceCurvature.h`, `BDPTVertexRIGRebuildTest.cpp` listed only two of the four normal-perturbing modifiers and now name all four. | *(this record)* |
| **P1-C** — `SceneEditorSuggestionsTest` hard-codes the registered-chunk count in two EXPECTs; `relief_modifier` made it 175 and the suite was red on exactly those two. | Both bumped 174 → 175, per-addition history extended.  Suite added to the Phase-1 gate list above, and flagged as a **Phase-2 gate** (`modifier_stack` → 176). | `c246ae8f` |
| **P2-1** — the `step` descriptor promised "set it explicitly when the field's features are finer than the floor", which cannot work: an explicit step is raised to the footprint too. | Descriptor rewritten: the explicit step is a **floor**; the footprint wins when larger, so on meshes relief fades toward flat at distance and a sub-footprint step is silently ignored; on analytic primitives and SDFs no footprint exists, the max is a no-op, there is **no fade**, and the explicit step is used verbatim.  Mirrored in §3.3 with the mesh-only disclosure.  **Code rule unchanged.** | `0bec7038`, doc in *(this record)* |
| **P2-2** — the `mag2` guard's comment claimed "a gradient large enough to cancel N", which is impossible (`T`,`B` ⟂ `N`, so `\|N − scale·g\|² = 1 + scale²\|g\|² ≥ 1`). | Comment rewritten: the guard catches only a non-finite `mag2` (redundant with the `isfinite` gate for finite inputs — exactly what red-proof (c) measured) and a zero-length `N` from a singular transform.  §12 deviation 4 updated to match. | `bc2b6a9b` |
| **P2-3** — test 4 did not discriminate `max(·, fw)`: `fbm` fades its own octaves, so the sequence stays monotone with the max deleted. | Added a second sweep on an fw-**blind** step height (`H = P.x > 0 ? 0.1 : 0`, probed at `x = 0`), where `\|N′−N\| ∝ 0.1/(2s)` and must therefore *strictly* decrease as `fw` grows.  Red-proof (e). | `44cf535d` |
| **P2-4** — the no-object-map warning read as a per-hit accident. | Reworded to lead with the truth (it is a property of the object; every CSG-composite hit lands here) and to name the affected authoring surfaces. | `bc2b6a9b` |
| **P2-5** — `ptCoord1` is not moved by the chain rule (only TEXCOORD_0 has `dpdu`/`dpdv`). | Disclosed in §3.2's point-domain list and on the `height` descriptor: a TEXCOORD_1-parameterised height is **not supported** — it reads flat in *both* domains — author against TEXCOORD_0 or use a 3D field in `surface`. | `0bec7038`, doc in *(this record)* |
| **P2-6** — the Phase-1 record did not mention the separately-landed migrator. | `c3600ed0` (`tools/migrate_scenes_relief.py`) named in the commit-count paragraph as the **Phase-3** deliverable it is. | *(this record)* |

**New red-proofs** (both mutations applied, observed red, reverted; recorded
in the test header):

- **(e) the `max(·, fw)` step rule.**  Deleting the
  `if( txFootprint.valid && worldWidth > s ) s = worldWidth` block leaves the
  fbm sweep **green** — confirming the reviewer's charge that it could not
  discriminate — and fails all three of the new step-height assertions
  (`|N′−N|` pinned at 1.4000 for every footprint instead of
  1.400 → 1.268 → 0.460 → 0.050).
- **(f) the frame-rebuild gate.**  Reverting all three modifiers to
  `ri.bHasShadingTangent` fails **5** assertions: 7b(i)'s ONB check for
  ReliefModifier, BumpMap *and* NormalMap, plus 7b(ii) and 7b(iii).  7b(i)'s
  `vNormal` checks stay green by construction — the gate decides only how the
  ONB is rebuilt, never the normal — which is precisely why a normal-only
  assertion could not have caught this bug.

**Gate suites, run on the final tree.**  `ReliefModifierTest` **80/0**,
`GlintModifierTest` ALL PASSED, `HairTangentPlumbingTest` **123/0** (its
tests 10 and 12 — the BoxGeometry control carrying *neither* flag — still
byte-match an independent `CreateFromW` golden, which is the invariant P1-A's
OR had to preserve), `GeometryShadingTangentTest` **12606/0**,
`SceneEditorSuggestionsTest` ALL PASSED (14 cases),
`SourceHygieneTest` **164/0**, `CstResolverTest` **44/0**,
`ScalarPainterParserTest` **60/0**.  Clean warning check on every changed
`.cpp` (touch + rebuild): **zero**.

**Residual closed in the same round.**  An SDF-heightfield hit used to take
`NormalMap`'s last-ditch T/B branch and fire its once-per-process "no tangent
frame" warning: the **values were unaffected** (that branch reads the same
`ri.onb.u()/v()` the coherent-tangent branch does).  The branch now gates on
`ModifierFrame::HasCoherentTangent` — the same predicate the rebuild uses — so
the diagnostic and the rebuild agree on what a coherent-tangent hit is.
Suites after the change: `ReliefModifierTest` 80/0, `GlintModifierTest` all
pass, `HairTangentPlumbingTest` 123/0.
*(Corrected, fix round 2 P2-B: the sentence originally here claimed the
retired warning "was a false positive on exactly that hit" — an
overstatement.  SDFGeometry's heightfield `ptCoord` is parameterised from
the OBJECT-space hit point, while the coherent tangent this branch reads is
built from a WORLD-X projection (Object::IntersectRay's no-supplied-tangent
fallback) — the two agree only when the instance's linear part maps
object +X to world +X, i.e. no rotation.  Suppressing the warning is a true
false positive only on an UNROTATED SDF-heightfield instance; on a ROTATED
one the tangent basis is genuinely misaligned with the heightfield's own
UV axes, and silence there is a disclosed diagnostic gap, not a corrected
false positive.  NormalMap.cpp's comment at the branch says so now.  Also
corrected: the last-ditch warning's text named the C++ symbol
`ModifierFrame::HasCoherentTangent` — reworded to "a geometry-supplied
shading tangent", matching the plain-author language the rest of that
warning already uses for the TANGENT/derivatives checks.)*

---

### Phase 1 — fix round 2 (2026-09-06)

Three independent reviewers on the fix-round-1 tree returned **1 P1 and 5
P2s**.  All six are fixed.  Suite after the round: `ReliefModifierTest`
**85/0** (was 80/0; test 4c is the addition, 5 new checks).

| Finding | Fix | Commit |
|---|---|---|
| **P1** — the frame-rebuild block's parenthetical (NormalMap.cpp ~210-220) described PRE-fix behaviour: it still said "the T/B selection above still keys on the bare flag" and that an SDF-heightfield hit "falls to the last-ditch branch", both true before `44cf535d` and false after it. | Rewritten to the truth: three tangent-source branches (imported TANGENT / UV derivatives / `ModifierFrame::HasCoherentTangent`) feed a rebuild gated on the same predicate; an SDF-heightfield hit now takes the third branch, and the last-ditch branch (and its warning) fires only on a genuinely tangent-less hit. | `b585d15b` |
| **P2-A** — `txFootprint.worldWidth` was stamped in OBJECT-space units by `ComputeTextureFootprint` (it runs mid-`Object::IntersectRay`, on the ray that function has already transformed into object space) but never folded to world units, unlike its sibling `derivatives.scaleHint`.  Every consumer (`fw`, ReliefModifier's `max(step, worldWidth)`) silently read object units on any non-unit-scale object. | Folded `worldWidth` by `m_worldLinearScale` at the same point `Object::IntersectRay` / `CSGObject::IntersectRay` fold `scaleHint` (same `\|det M\|^(1/3)` approximation, exact under uniform scale).  Doc comments in `RayIntersectionGeometric.h`, `TextureFootprintCompute.h` and §3.3 corrected to say the geometry stamps object units and the Object layer folds world scale.  New regression: `ReliefModifierTest` test 4c — a real cast through `Object::IntersectRay` (single-triangle mesh, ray differentials) at world scale 1 vs. scale 10 (uniform), same local hit point and local diffs by construction, must give a `worldWidth` ratio of 10.  Red-proof (g): disabling the fold makes the ratio read 1. | `667e6856` (Object.cpp/CSGObject.cpp/headers), `c0bba520` (test 4c), `f34156b6` (§3.3 doc) |
| **P2-B** — NormalMap.cpp's coherent-tangent-branch comment and this record's "Residual closed in the same round" paragraph both claimed the retired warning "was a false positive on exactly that hit" for the SDF-heightfield case — an overstatement. | Both rewritten: the coherent tangent there is a WORLD-X projection (Object.cpp's no-supplied-tangent fallback) while the heightfield's own UV is parameterised from the OBJECT-space hit point (SDFGeometry.cpp's `m_isHeightfield` branch) — they agree only when the instance's linear part preserves world-X, i.e. no rotation.  Suppressing the warning is a true false positive only on an unrotated instance; on a rotated one the diagnostic gap is real and now disclosed, not hidden.  Also reworded the warning text itself, which named the C++ symbol `ModifierFrame::HasCoherentTangent`, to plain author language. | `b585d15b` (NormalMap.cpp), `f34156b6` (doc) |
| **P2-C** — `ReliefModifierTest` test 7's fixture set `bHasShadingTangent = true` without `bShadingTangentFromGeometry`, a combination no in-tree geometry produces (every producer sets both). | Both flags set at both fixture sites.  `ReliefModifierTest` confirmed 80/80 before this round's other additions, 85/85 after. | `c0bba520` |
| **P2-D(i)** — the design doc's status line still said "DESIGN (2026-09-05)" after Phase 1 shipped and went through a full fix round. | Changed to "Phase 1 LANDED (2026-09-06, two review rounds to zero P1 — see §12); Phases 2–5 pending." | `f34156b6` |
| **P2-D(ii)** — `ModifierFrame.h` and this record's P1-A row cited "SDFGeometry.cpp:1599" for the `bShadingTangentFromGeometry = true` statement; the line is 1600. | Both re-cited by symbol ("SDFGeometry.cpp, the `m_isHeightfield` branch of `IntersectRay`") instead of a line number, so a future edit can't put it out of date the same way; the same stale line number in `ReliefModifierTest.cpp`'s test-7b fixture comment was swept too. | `8c23185e` (ModifierFrame.h), `f34156b6` (doc), `c0bba520` (test) |

**Gate suites, run on the final tree.**  `ReliefModifierTest` **85/0**,
`GlintModifierTest` ALL PASSED, `HairTangentPlumbingTest` **123/0**,
`GeometryShadingTangentTest` **12606/0**, `SurfaceCurvatureTest` **94/0**,
`PainterPreviewTest` **87/0**, `TexCoord1PainterTest` **33/0**.  Clean
warning check on every changed `.cpp` (touch + rebuild): **zero** —
`Object.cpp`, `CSGObject.cpp`, `NormalMap.cpp`, `ReliefModifierTest.cpp`.

**Red-proof.** (g) Test 4c, the `worldWidth` object-to-world fold: disabling
the `ri.geometric.txFootprint.worldWidth *= m_worldLinearScale` line added to
`Object::IntersectRay` (via a dead `if( false && ... )` guard, rebuilt, run,
reverted) makes test 4c's ratio read exactly `1` instead of `10`, and the
suite goes 84/85.  Reverted; suite back to 85/85.

---

### Phase 1 — fix round 3 (2026-09-06)

Three independent reviewers on the fix-round-2 tree returned **1 P1 and 8
P2s**, all citation/wording — no code-behaviour change in this round.  Suite
after the round: `ReliefModifierTest` **85/0**, unchanged (comment/doc-only).

| Finding | Fix | Commit |
|---|---|---|
| **P1** — `ModifierFrame.h`'s `bShadingTangentFromGeometry`/`bHasShadingTangent` pairing comment, and the matching comment in `ReliefModifierTest.cpp`'s test-7b fixture, cited `RayIntersectionGeometric.h:391-394` — this arc's own edits had already moved that text to ~405-409. | Both re-cited by symbol: "the `vShadingTangent` / `bHasShadingTangent` field comment in `RayIntersectionGeometric.h`", so a future edit can't put the citation out of date again. | *(this record)* |
| **P2-1** — the design doc's §2 geometric-normal row cited `RayIntersectionGeometric.h:153-176`, which had moved to ~168-191. | Re-cited as "the `vGeomNormal` field comment in `RayIntersectionGeometric.h`". | *(this record)* |
| **P2-2** — the fix-round-1 P1-B row (§12) cited `RayIntersectionGeometric.h:151`/`:158` for the same enumeration family. | Re-cited as "the `vNormal` and `vGeomNormal` field comments in `RayIntersectionGeometric.h`". | *(this record)* |
| **P2-3** — `docs/CLOTH_FABRIC_DESIGN.md`'s `TextureFootprint` reference cited `RayIntersectionGeometric.h:116-127`, which had moved to ~131. | Re-cited as "the `TextureFootprint` struct in `RayIntersectionGeometric.h`". | *(this record)* |
| **P2-4** — `docs/GEOMETRY_SHADING_SIGNALS_DESIGN.md` cited `ExpressionPainter.cpp:39` for `ctx.fw = ...`, which is actually at two sites (~114, ~256), neither line 39. | Re-cited as "`ExpressionPainter::BuildContext` / `ExpressionScalarPainter::BuildContext` in `ExpressionPainter.cpp`". | *(this record)* |
| **P2-5** — `Object.h`'s `m_worldLinearScale` field comment enumerated only `derivatives.scaleHint` and `derivatives.curvature` as fold targets, omitting `txFootprint.worldWidth` (folded in `Object::IntersectRay` / `CSGObject::IntersectRay` since fix round 2). | Enumeration extended to name `txFootprint.worldWidth`. | *(this record)* |
| **P2-6** — `NormalMap.cpp`'s coherent-tangent parenthetical said "(uniform or non-uniform SCALE and translation are fine)", which is false under an orientation-reversing (negative) scale: `scale -1 1 1` / a mirror flips object +X to world −X while `Object.cpp`'s mirroring correction only flips V, leaving U — and therefore the coherent-tangent projection — silently wrong. | Reworded to "no rotation, shear, or orientation-reversing (negative) scale; a positive uniform or non-uniform scale and translation are fine." | *(this record)* |
| **P2-7** — the `|det|^(1/3)` fold's geometric-mean-approximation disclosure (`Object.cpp`'s `worldWidth` fold comment, and design §3.3) did not say the approximation can be worse than no fold at all on a strongly anisotropic instance. | One sentence added to each: on a flattened/elongated instance (e.g. `scale 4 0.05 4` on a panel: in-plane scale 4x, `|det|^(1/3) = 0.928`, a 4.31x under-count) the fold under-scales `worldWidth`; the error only under-filters, so `max(step, worldWidth)` falls back to `step` — the same aliasing as pre-fix, never worse than that floor.  Uniform scale remains exact. | *(this record)* |
| **P2-8** — `docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md` (2026-08-20) and `skills/agent/procedural-textures.md` both asserted `fw` is "a real WORLD-space filter-width estimate" without qualifying that this was true only for unscaled mesh instances before the fix-round-2 `worldWidth` fold. | One dated caveat sentence added to each (surrounding text left as-is): on a SCALED mesh instance, `fw` was OBJECT-space until 2026-09-06 (relief-modifier arc, §3.3) and is world-correct since. | *(this record)* |

**Gate suites, run on the final tree.**  `ReliefModifierTest` **85/0**
(unchanged — no executable line touched).  Clean warning check on the two
touched `.cpp` files (touch + rebuild): **zero**.

---

### Phase 1 — fix round 4 (2026-09-06)

A converging round: two findings, both citation/wording — no code-behaviour
change. Suite unaffected: `ReliefModifierTest` **85/0**, unchanged.

| Finding | Fix | Commit |
|---|---|---|
| **status-line count** — the doc's top status line said "two review rounds to zero P1" after fix rounds 1–3 had already landed (three rounds by then). | Reworded to "four review rounds" (this round being the fourth). | `4f180cc0` |
| **CSGObject/Object citations** — the "Inputs" line's hook-timing citation `Object.cpp:658-977`, the §2 hook-timing row's matching citation, and the §2 "one modifier per object" row's `CSGObject.cpp:1612` had all drifted off their described content as earlier rounds' insertions shifted line numbers. | Re-cited by symbol: `Object::IntersectRay through its `ri.pModifier = pModifier` assignment` (both hook-timing sites) and `the `if( pModifier )` override block in `CSGObject::IntersectRay`` (the one-modifier-per-object row). | `4f180cc0` |

**Gate suites, run on the final tree.**  `ReliefModifierTest` **85/0**
(unchanged — doc-only). Clean warning check: not applicable (no `.cpp`
touched).

---

### Phase 2 — reviewed 2026-09-06 (one adversarial round, 0 correctness P1s; R9 CLEAN incl. `leaks --atExit` on nested stacks; 2 citation P1s fixed)

Branch `relief-modifier`, three commits off `cf6a363a` (fix round 3's head):
`1b2851bf` (the class + all wiring + the two chunk-count bumps),
`673b7a96` (`ReliefModifierTest` test 9), `c1fd3076` (`CstResolverTest`'s
repeatable-rename case).  **This slice has now been through one
implementation-review-loop adversarial round**: 0 correctness P1s (reviewer
R9 ran `leaks --atExit` against the nested-stack fixture and reported
CLEAN, closing the self-audit's weakest-verified item below) and 2 citation
P1s (both from `ChunkParserRegistry.cpp` line ranges that `ModifierStackAsciiChunkParser`'s
own insertion shifted — see "Phase 2 — fix round 1" immediately below).

**Files.** New: `src/Library/Modifiers/ModifierStack.{h,cpp}`,
`scenes/Tests/ChunkCoverage/cc_modifier_stack.RISEscene`.  Modified:
`RISE_API.{h,cpp}`, `IJob.h`, `Job.{h,cpp}`, `ChunkParserRegistry.cpp`,
`Parsers/README.md`, `tests/IJobVtableManifest.txt`,
`tests/SceneEditorSuggestionsTest.cpp` (174/175→176 twice, per-addition
history extended in both places), `tests/ReliefModifierTest.cpp` (test 9 +
red-proof (h) documented in the file header), `tests/CstResolverTest.cpp`
(the `[modifier-stack-repeatable-rename]` case), and the five build
projects (`build/VS2022/Library/Library.vcxproj{,.filters}`,
`build/XCode/rise/rise.xcodeproj/project.pbxproj`,
`build/cmake/rise-android/rise_sources.cmake`, `build/make/rise/Filelist`).

**Deviations from the design, with reasons.**

1. **`IJob::AddModifierStack` takes `const char** modifierNames`, not the
   charter's `const char* const*`.**  Both types pass the same data; `const
   char**` is the existing convention for every other IJob virtual that
   carries a repeatable name array (`AddStandardShader`, `AddAdvancedShader`,
   `AddVoronoi2DPainter`, `AddVoronoi3DPainter`, `AddPiecewiseLinearFunction2D`)
   — matching it keeps `ModifierStackAsciiChunkParser::Finalize` byte-for-byte
   the same shape as `StandardShaderAsciiChunkParser::Finalize` (both build a
   flat `char* mem` block + a `char**` of offsets into it from
   `bag.GetRepeatable(...)`), rather than introducing a one-off calling
   convention for a single virtual.  `tests/IJobVtableManifest.txt`'s new
   line was hand-derived against `NormalizeDeclSignature`'s tight-char rule
   and confirmed by `SourceHygieneTest` (164/0, no manifest mismatch).
2. **Parameter order is `(name, modifierNames, count)`**, matching the
   charter, not `AddStandardShader`'s `(name, count, shaderops)`.  Both
   orders appear in `IJob.h` already (compare `AddVoronoi2DPainter`'s
   `(name, pt_x, pt_y, painters, count, ...)` — count AFTER the array — with
   `AddStandardShader`'s count-before-array); there is no single house style
   to defer to, so the charter's explicit order was kept verbatim.
3. **The empty-stack and unknown-member diagnostics live in
   `Job::AddModifierStack`, not the chunk parser**, mirroring
   `Job::AddReliefModifier`'s "one home for the wording" precedent (its own
   comment cites the same reasoning) — `RISE_API_CreateModifierStack` ALSO
   rejects `count == 0` / a null member independently, so an out-of-tree
   caller that skips `Job` entirely (a hypothetical future scripting binding)
   still cannot construct a broken stack; the two checks are intentionally
   redundant, not duplicated wording.
4. **No `ModifierFrame.h`-style hoist was needed.**  Unlike Relief/BumpMap/
   NormalMap, `ModifierStack::Modify` does no frame rebuild of its own — it
   is a pure dispatch loop — so there is nothing to hoist.  The five-build-
   project entries therefore mirror `GlintModifier`'s two-file shape (`.h` +
   `.cpp` only), not `ReliefModifier`'s three-file shape (`.h` + `.cpp` +
   the shared `ModifierFrame.h`).  Fresh Xcode IDs were generated by
   sampling random 24-hex strings and grepping the whole `project.pbxproj`
   for each candidate before use (see the self-audit below).
5. **Test 9(b)'s probe point is hunted, not fixed.**  The design's item 9
   just says "glint-last… produces the same result as applying the prefix
   then Glint" — read literally that holds even when Glint is a no-op at
   the probe (still bit-identical, just not discriminating).  Measuring
   red-proof (h) against the first fixed probe tried (`(0.7, 0.2, 0)`)
   showed exactly that: the comparison stayed GREEN under a reversed apply
   order because Glint found no facet there.  The final test therefore
   searches a small grid via `GlintModifier::FindFacet` for a probe that
   actually lands on a facet, so the assertion is load-bearing rather than
   vacuously true — the search and its rationale are in the test's own
   comment, not asserted by fiat here.

**Red-proof.** (h) Reversing `ModifierStack::Modify`'s iteration
(`members.begin()/end()` → `members.rbegin()/rend()`, rebuilt, ran the
suite, reverted) fails 9a's two hand-chain comparisons and 9b's glint-last
comparison — **3 failures**, `ReliefModifierTest` 98/101.  9c (nesting)
stays GREEN under the same mutation, and that is a measured structural
fact, not a test gap: a globally-consistent order reversal commutes with
nesting (`stack{A, stack{B,C}}` and `stack{A,B,C}` both reverse to the
identical `C, B, A` application order), so 9c cannot distinguish forward
from reversed application by construction — only 9a/9b (against real,
non-order-symmetric modifiers) do that.  This is recorded in the test
file's own red-proof-(h) comment, corrected from an earlier draft of that
comment (written before the fixture existed) that predicted 9c would also
fail.  Reverted; suite back to 101/101.

**CST.**  No `Cst.cpp` change, confirmed rather than assumed: the new
`[modifier-stack-repeatable-rename]` case in `CstResolverTest` proves the
existing generic per-occurrence machinery (`ComputeChunkRefs` keys every
repeatable Param's edge by `(chunk, role, occurrence-index)` via
`DocParamId`; `DocRename` rewrites each referrer edge at its own recorded
`(chunk, role, occ)`) already gives `modifier_stack`'s `modifier` param —
`ValueKind::Reference`, `referenceCategories = {Modifier}`,
`p.repeatable = true`, the same declaration shape as `standard_shader`'s
`shaderop` — correct per-occurrence rename: renaming one member of a
two-member stack rewrites only that occurrence's line, a control renaming
the OTHER member rewrites only the other line, and neither rename touches
its sibling.  `CstResolverTest` **52/0** (was 44/0; +8 checks, one test
block).

**Gate suites, run on the final tree.**  `ReliefModifierTest` **101/0** (was
85/0; test 9 is the addition, 16 checks net — 15 order/nesting/parse checks
plus the `foundFacet` setup assertion for 9b's facet hunt).
`GlintModifierTest` ALL PASSED, `SceneEditorSuggestionsTest` ALL PASSED (13
cases; both hard-coded counts now 176), `SourceHygieneTest` **164/0**,
`CstResolverTest` **52/0**, `CstRecordDeriveTest` **23/0**,
`ScalarPainterParserTest` **60/0**.  Clean warning check on every touched
`.cpp` (touch + rebuild, full-project `make -C build/make/rise -j8 all`
plus each gate test binary): **zero**.  `cc_modifier_stack.RISEscene`
parses headlessly (`printf "quit\n" | ./bin/rise ...`) with zero
errors/warnings in `RISE_Log.txt`.  `CstDeriveGoldenTest` after adding the
scene: **436 MATCH, 1 DRIFT, 3 UNCOVERED, 0 STALE** (of 447 corpus scenes)
— the 1 DRIFT is the pre-existing, out-of-scope
`scenes/FeatureBased/BDPT/bdpt_crystal_garden.RISEscene` edit (confirmed
unrelated: it was already uncommitted and flagged untouchable before this
phase began); the 3 UNCOVERED are the two Phase-1 scenes (already
UNCOVERED before this phase, per that record) plus the new
`cc_modifier_stack.RISEscene` — **expected, not regenerated**, per §11's
Phase-3 golden-regen deliverable; do not regenerate here.

**Self-audit (the 5 likeliest ways this is wrong, and what was checked).**

1. **Refcount on nested stacks (double-free / leak).**  `ModifierStack`'s
   ctor addrefs every member including a member that is itself another
   `ModifierStack`; the dtor releases every member once.  Checked: test 9c
   builds `stackBC` (owning B, C) and a separate `nested` stack whose
   second member IS `stackBC` (addref'd again by `nested`'s ctor) — so
   `stackBC` ends the test with refcount 2 (one from the test harness's
   `Own()`, one from `nested`).  `ReleaseOwned()` releases the harness's
   copy; `nested`'s dtor (never explicitly invoked in this test, since
   `Own()` keeps objects alive to process exit) still holds its copy.  No
   crash, no ASan/valgrind run in this session — the test suite has no
   sanitizer build wired into this gate list, so this was checked by
   construction (addref count == number of stacks holding a pointer, release
   count == same) and by absence of a crash across 101 checks, not by an
   instrumented tool, when this record was first written.  **Closed by the
   Phase 2 review round (R9): an instrumented `leaks --atExit` run against
   the nested-stack fixture reported CLEAN**, so this item is no longer the
   weakest-verified on this list.
2. **Repeatable-param reading order.**  `bag.GetRepeatable("modifier")`
   returns values "in input order" (its own doc comment); test 9e's
   two-member scene (`modifier r1` then `modifier g1`) round-trips through
   the real chunk parser and `CstResolverTest`'s occ0/occ1 checks confirm
   occurrence 0 is `r1` and occurrence 1 is `g1` in the PARSED document —
   checked against the actual parser, not assumed from the accessor's
   contract alone.
3. **CST closure/rename** — see the CST section above; verified with a new
   test rather than inspected by reading code only, including a reverse
   control (renaming the second member) to rule out "first occurrence
   always wins" as a passing-by-accident explanation.
4. **Vtable manifest line correctness.**  `NormalizeDeclSignature`'s
   tight-char collapse was traced BY HAND against the exact declaration
   text before writing the manifest line (documented turn-by-turn in this
   session, not reproduced here), then confirmed by building and running
   `SourceHygieneTest`, which parses the ACTUAL `IJob.h` text through the
   same function and diffs it against the manifest line — 164/0, no
   "IJob SIGNATURE CHANGE" or count-mismatch diagnostic fired, which is the
   test that would have caught a hand-derivation error.
5. **Xcode IDs.**  Six fresh 24-hex-uppercase IDs were generated and each
   was grepped against the FULL `project.pbxproj` text before insertion
   (a Python one-shot that only accepts an id absent from the file);
   post-edit `grep -n ModifierStack` on the final file shows exactly six
   occurrences in the expected four sections (PBXBuildFile ×4,
   PBXFileReference ×2 — folded into the same six-line grep because two
   IDs each appear twice, once as their own key and once referenced by a
   `fileRef =`), matching GlintModifier's own six-reference shape line for
   line.  Not verified: an actual Xcode/xcodebuild parse of the project
   file (no macOS Xcode toolchain invocation was run in this session) —
   the ID-uniqueness and structural-mirroring checks are textual, not a
   build verification.

**Left undone, deliberately.**  `tests/data/cst_derive_golden.txt`
is NOT regenerated (Phase 3's job, same as Phase 1).  No render/pixel
verification of `cc_modifier_stack.RISEscene` was performed beyond a
headless parse (Phase 5's job, per §11).

---

### Phase 2 — fix round 1

One adversarial round on the Phase 2 tree returned **0 correctness P1s**
(reviewer R9 additionally ran `leaks --atExit` against the nested-stack
fixture and reported CLEAN, closing self-audit item 1's residual above)
and **2 citation P1s**, both a consequence of `ModifierStackAsciiChunkParser`'s
own insertion shifting `ChunkParserRegistry.cpp` line ranges that earlier
doc text had cited numerically.  Both are fixed.  Suite unaffected:
`ReliefModifierTest` **101/0**, unchanged (doc-only).

| Finding | Fix | Commit |
|---|---|---|
| **P1 (citation)** — §4's `modifier_stack` composition section cited `ChunkParserRegistry.cpp:10918` as the `standard_shader.shaderop` repeatable-Reference precedent; that line is inside an unrelated area-light shader op, not `StandardShaderAsciiChunkParser`. | Re-cited by symbol: "the `shaderop` parameter in `StandardShaderAsciiChunkParser::Describe`". | *(this record)* |
| **P1 (citation)** — the top "Inputs" line cited `ChunkParserRegistry.cpp:8534-8690` as spanning the three pre-existing modifiers' descriptors; `ModifierStackAsciiChunkParser` (Phase 2) now sits between `ReliefModifierAsciiChunkParser` and `GlintModifierAsciiChunkParser` in that range, so the cited span no longer covers only the three named parsers. | Re-cited by symbol: `BumpmapModifierAsciiChunkParser`, `NormalMapModifierAsciiChunkParser`, `GlintModifierAsciiChunkParser`, all in `ChunkParserRegistry.cpp`. | *(this record)* |

---

### Phase 3 — implemented 2026-09-06 (review round 1: see fix round 1 below)

Branch `relief-modifier`, four commits off `4f180cc0` (round-4 fix's head):
`658c6c94` (corpus migration), `e690f4de` (teaching surfaces + §7.4
recount), `8620d9cf` (CST test twins), `eefc8e1a` (deprecation diagnostic +
`ReliefModifierTest` test 10f). This slice has since been through one
implementation-review-loop round: three independent reviewers, spanning
code correctness, the teaching surfaces (parseability + accuracy), and
migration losslessness, returned 4 P1s and a set of P2s — see "Phase 3 —
fix round 1" immediately below for the findings and their fixes.

**Corpus migration** (`tools/migrate_scenes_relief.py --root scenes -v`,
`--selftest` passing first). All 5 in-tree `bumpmap_modifier` chunks
migrated in 4 files, matching the design's §7.3 table exactly:

| Scene | `S'` | Tracked? |
|---|---|---|
| `scenes/FeatureBased/Materials/velvet_cushion.RISEscene` (`creases`, normalized) | `-0.0075` | yes |
| `scenes/FeatureBased/Combined/sculptors_studio.RISEscene` (`clay_bump`) | `-0.005` | yes |
| `scenes/Tests/SMS/sms_veach_egg_bumpmap.RISEscene` (`bump_egg`) | `-0.02` | yes |
| `scenes/Internal/pool.RISEscene` (`bumpchecker`, `waterbump`) | `-0.001`, `-0.0008` | no (gitignored) |

A second run over the migrated tree is a no-op (idempotency confirmed:
"0 bumpmap_modifier chunks seen"). Each migrated scene's own header/section
comments that named `bumpmap_modifier` as the mechanism were updated to
name `relief_modifier` (migrated, `domain uv`) so the scene's own text is
not stale; `velvet_cushion`'s "A BUMP map, not a displacement" concept
comment (quoted in `CLOTH_FABRIC_DESIGN.md`) was deliberately left as-is —
it describes the technique category, not the deprecated chunk name, and
remains accurate for `relief_modifier` too. `bdpt_crystal_garden.RISEscene`
carries a pre-existing, unrelated uncommitted edit and was left untouched
throughout, per the branch note in §11.

**Teaching surfaces**, per §9's placement law: `procedural-textures.md`
gets the full reference — a rewritten `expression_function2d` decision-map
row (names DISPLACEMENT as its sole remaining niche, states the
deprecation + migrator), a new `relief_modifier` decision-map row, and a
new "Adding relief" section (height/scale/domain/step, sign convention,
`modifier_stack` ordering with glint last, the deprecation note).
`materials-and-media-basics.md` gets the ONE worked, execution-validated
example — a crackle-glaze ceramic where a single `expression_painter`
Worley `f2-f1` cell field drives colour (`ramp_painter`), roughness
(`scalar_painter { painter ... channel R }`) and relief (`relief_modifier`
over the SAME `scalar_painter`, negative scale so the cracks sink).
`object-modeling-recipes.md` gets one pointer at Recipe 6's
lathe/sweep/skin faceting bullet: `relief_modifier` cannot introduce that
faceting because it never moves a vertex. `src/Library/Parsers/README.md`'s
Modifiers row and `docs/GLTF_IMPORT.md`'s normal-map section both now name
the deprecation and the migrator. `docs/MATERIALS.md` has no `bump`
mention at all (grepped, confirmed, nothing to change).
`CLOTH_FABRIC_DESIGN.md` and `GEOMETRY_SHADING_SIGNALS_DESIGN.md` were
deliberately NOT rewritten — both are historical/architecture records
(a past-tense finding, and a citation of `BumpMap.cpp`/`NormalMap.cpp` as
implementation precedent), not living recommendations of `bumpmap_modifier`
as the forward path.

**§7.4 post-migration recount**, by a scene-parsing script (matching each
`displaced_geometry`/`function2d_painter`/`scalar_painter`/
`composite_function2d_painter`/`sdf_geometry` binding against the
`expression_function2d` names actually declared in the SAME file, not a
bare grep for the string): `displaced_geometry.displacement` 5 scenes,
`function2d_painter` 4, `scalar_painter { function2d }` 2 (`watch_dial`
plus, new this phase, `velvet_cushion`), `composite_function2d_painter` 0
direct children, `sdf_geometry.heightfield_function` 5. Zero
`bumpmap_modifier` chunks remain anywhere under `scenes/`. The
shrinking-legacy-surface claim is confirmed: SIX chunk kinds could bind
an `expression_function2d` before this arc (the five above plus
`bumpmap_modifier.function`); FIVE can after Phase 3.

**Deprecation diagnostic** (`BumpmapModifierAsciiChunkParser::Finalize`,
`ChunkParserRegistry.cpp`): a `static std::atomic<bool>` once-per-process
guard (the `NormalMap.cpp` idiom) emits `eLog_Warning` naming
`relief_modifier` and `tools/migrate_scenes_relief.py`; the descriptor's
`description` is prefixed `"DEPRECATED — use relief_modifier (...)"`.
ABI freeze holds: `IJob::AddBumpMapModifier`, the `RISE_API_CreateBumpMapModifier{,Ex}`
entry points, and `BumpMap.{h,cpp}` are untouched. `ReliefModifierTest`
gains test 10(f): two `bumpmap_modifier` chunks in one scene parse
successfully and the captured log contains the deprecation text EXACTLY
ONCE — the ordering note in the test's own comment records why this must
stay the first ASCII-parsed `bumpmap_modifier` chunk in the process
(Test 2's legacy-equivalence check constructs a `BumpMap` directly in
C++, bypassing the chunk parser, so it never trips the guard).

**CST test twins.** All four bumpmap_modifier CST fixtures kept (they test
the deprecated-but-still-parsing path) and gained a `relief_modifier`
twin proving the same closure/derive/removal/instance-inheritance logic on
the new slot: `CstResolverTest`'s `[relief-modifier-scalar]` (closure of a
`scalar_painter` includes the `relief_modifier` naming it — the
new-modifier analogue of `[painter-decl-func2d]`'s
`bumpmap_modifier.function`/Function2D case, one manager over);
`CstRecordDeriveTest`'s twin scene in the record/derive cross-check array;
`CstIncrementalSafetyTest`'s twin OPTIONAL-SLOT REMOVAL block; and
`CstSourceInstanceTest`'s twin `[inherit]` case (its shared `Scene()`
fixture gained a `scalar_painter`/`relief_modifier` pair alongside the
legacy `bump` fixture, which shifted one hardcoded chunk-ordinal pair in
an unrelated `[refuse]` test from `#11`/`#12` to `#13`/`#14` — confirmed
against the actual diagnostic text via a temporary probe print, not
guessed, then fixed in the same commit). `relief_modifier.height` binds a
`scalar_painter`, not the colour painter `bumpmap_modifier.function` uses
(a colour painter is refused there), so each twin adds a small dedicated
`scalar_painter` rather than reusing the bumpmap fixture's own painter.

**Gate suites, run on the final tree.** `ReliefModifierTest` **106/0**
(was 101/0; +5 checks, test 10(f)). `CstResolverTest` **53/0** (was 52/0).
`CstRecordDeriveTest` **25/0** (was 23/0). `CstIncrementalSafetyTest`
**41/0**. `CstSourceInstanceTest` **457/0** (was 456/0 before the
chunk-ordinal fix). `SceneEditorSuggestionsTest` ALL PASSED (both
hard-coded chunk counts still 176 — Phase 3 adds no chunk).
`SourceHygieneTest` **164/0**. `ScalarPainterParserTest` **60/0**. Clean
warning check on every touched `.cpp` (touch + rebuild, full-project
`make -C build/make/rise -j8 all` plus each gate test binary): **zero**.

**Golden regen** (the ls-files lesson: every new/changed scene `git add`-ed
first). `CstDeriveGoldenTest --generate` then `git diff
tests/data/cst_derive_golden.txt`: additions-only for the 4 new scenes
carried over from Phases 1–2 that had been UNCOVERED
(`cc_modifier_stack.RISEscene`, `cc_relief_modifier.RISEscene`,
`relief_sphere_no_uv.RISEscene`) plus this phase's new
`relief_crackle_glaze.RISEscene`, and the pre-existing, out-of-scope
`bdpt_crystal_garden.RISEscene` DRIFT line — restored to HEAD's digest
value (`2e24838a...`, `7406` bytes) after the generate step overwrote it
with the dirty working tree's digest (`e70506d7...`, `7411` bytes), per
the branch note. `./bin/tests/CstDeriveGoldenTest` (verify mode): **440
MATCH, 1 DRIFT** (the restored `bdpt_crystal_garden` line, expected) **(of
441 golden scenes); coverage: 448 corpus scenes, 0 UNCOVERED, 0 STALE**.

Importantly, the 3 migrated in-tree scenes (`velvet_cushion`,
`sculptors_studio`, `sms_veach_egg_bumpmap`) show **NO digest change** —
this is correct, not a gap: `DumpJob` (the golden's hashed canonical form)
prints an object's modifier binding as `modifier=<name>`
(the `modifier=` line in `DumpJob` (`CstRenderEquivalence.h`), a reverse-name lookup), never the bound
modifier's concrete type or parameters, and the migrator does not touch
the object's `modifier N` line (§7.2) — so a scene whose `bumpmap_modifier
creases` became a `relief_modifier` of the same name dumps byte-identical
either way. The digest is a structural (by-name) equivalence check, not a
behavioral one; this was verified by reading `CstRenderEquivalence.h`
before treating "no diff" as suspicious.

**Render evidence.**
`rendered/relief_crackle_glaze.png` (256×256, 32 spp, PT, `oidn_denoise
FALSE`) — cracks read as sunken relief (confirmed by a same-seed A/B in
the scratchpad: `relief_modifier.scale 0.0` vs the authored `-0.02`
produces a measurable, sphere-confined shading difference, mean abs diff
0.895/255 over the full frame, max 68/255 at crack edges; a 3× zoomed crop
at each setting shows the `-0.02` version's crack edges catching a thin
specular bevel the flat version lacks). The crack width (`smoothstep`
edge 0.06 → 0.10) and relief amplitude (`scale` −0.006 → −0.02) were both
widened from the first draft after the first render read too subtly at
256×256 — recorded here so a future viewer of this file's history is not
confused by the two values appearing in early session output.
`velvet_cushion` lossless-migration check: master's version (`git show
4f180cc0:scenes/FeatureBased/Materials/velvet_cushion.RISEscene`) and the
migrated tree version, both reduced to 256×256/16spp in the scratchpad
(the authored 1200×900/128spp would not fit the time budget), rendered
~5s each — mean luminance 77.74 (master) vs 77.72 (migrated), mean abs
diff 0.26/255, both frames showing the same radial nap-crease gathering
around the crown; the residual is ordinary MC noise between two
differently-seeded renders (renders seed from wall clock per the PT
env-MIS arc), not a migration artifact.

**Self-audit (the 5 likeliest ways this is wrong, and what was checked).**

1. **Migrated look changed.** Checked by the `velvet_cushion` A/B above
   (mean luminance within 0.03%, same visible crease pattern) rather than
   assumed from the algebra alone.
2. **Golden "no diff" on the 3 migrated scenes hides a real gap.** Checked
   by reading the `modifier=` line in `DumpJob` (`CstRenderEquivalence.h`) and confirming it only
   ever prints a modifier binding by NAME — a structural fact, not an
   assumption — so the absence of a digest change is the CORRECT result
   given what the golden actually hashes, not evidence the migration did
   nothing.
3. **The once-per-process warning test ordering.** `ReliefModifierTest`'s
   own file was grepped for every `bumpmap_modifier` occurrence before
   writing test 10(f); the only other one (Test 2) goes through a direct
   C++ constructor, confirmed by reading its body rather than assumed
   from the test name.
4. **A skill example that does not parse.** The crackle-glaze scene is a
   real, tracked, headlessly-parsed-and-rendered `.RISEscene`
   (`scenes/Tests/Painters/relief_crackle_glaze.RISEscene`), not prose —
   RISE_Log.txt shows zero warnings/errors across three separate renders
   (the shipped settings plus two scratch A/B variants) and the pasted
   copy in `materials-and-media-basics.md` was kept byte-identical to the
   file's own chunks after the crack-width/scale revision.
5. **A historical doc rewritten that should have stayed.**
   `CLOTH_FABRIC_DESIGN.md` and `GEOMETRY_SHADING_SIGNALS_DESIGN.md` were
   read and deliberately left untouched (see "Teaching surfaces" above);
   `docs/MATERIALS.md` was grepped and confirmed to have no `bump`
   mention at all, so there was nothing to touch there either.

**Left undone, deliberately.** Phase 4 (`DESIGN_FLAT_RELIEF` advisory +
hook-point notes) and Phase 5 (the `weathered_workbench` before/after pixel
verification named in §11) are untouched, per the phase boundaries in §11.
(The implementation-review-loop round this record originally flagged as not
yet run has since run — see "Phase 3 — fix round 1" below.)

---

### Phase 3 — fix round 1 (2026-09-06)

Three independent reviewers on the Phase 3 tree, one per lens (code
correctness, the teaching surfaces' parseability/accuracy, migration
losslessness), returned **4 P1s and 5 P2s**. All nine are fixed.

| Finding | Fix | Commit |
|---|---|---|
| **P1-1** — the crackle-glaze recipe in `materials-and-media-basics.md` and the relief section in `procedural-textures.md` each used the inline `keyword { params }` brace form in a fenced scene example, which the CST parser hard-rejects ("chunk braces must be on their own lines"): `uniformcolor_painter { name cg_f0  color 0.04 0.04 0.04 }` and `scalar_painter { name h2  painter some_colour_painter  channel R }`. | Both expanded to the multi-line form. Execution-validated: extracted both fenced blocks into scratch scenes (adding the minimal missing geometry/camera/film/light each block's own prose said it omitted — a sphere, a pinhole camera, a directional light, and a stand-in `uniformcolor_painter` for `some_colour_painter` in the second case), parsed headlessly, zero derive diagnostics in `RISE_Log.txt` for either. The crackle-glaze excerpt was re-diffed against `scenes/Tests/Painters/relief_crackle_glaze.RISEscene` chunk-by-chunk: parameter-for-parameter identical (byte-identical on the fixed `uniformcolor_painter` block). A grep of both files for any other `{ name` on one line found none. | `0a7892b6` |
| **P1-2** — the "`step` is auto by default … relief fades toward flat at distance" sentence in `procedural-textures.md` stated the footprint fade as universal, but only triangle-mesh geometry populates `txFootprint` today. | Added the mesh-only caveat verbatim from design §3.3: on analytic primitives and SDFs there is no distance fade at all, and the step used is just the `1e-3` floor or the explicit `step`. | `0a7892b6` |
| **P1-3** — §7.3's `tests/data/cst_derive_golden.txt` row promised "the migrated entries' digests" would change; they don't, because `DumpJob` (`tests/CstRenderEquivalence.h`) records an object's modifier binding by NAME only (`modifier=<name>`), never the bound modifier's type or parameters, and the migrator never touches the object's `modifier N` line. | Row rewritten to state the truth (additions-only for new scenes, zero digest change on migrated ones, and why), cross-referencing §12's Phase 3 record where the actual regen run confirms it. | *(this record)* |
| **P1-4** — §7.4 and its verbatim copy in §12 both said "FIVE chunk kinds could bind `expression_function2d`… after Phase 3, exactly FOUR", but the enumeration right above lists FIVE non-`bumpmap_modifier` kinds (`displaced_geometry.displacement`, `function2d_painter`, `scalar_painter { function2d }`, `composite_function2d_painter`, `sdf_geometry.heightfield_function`) — six before the arc, five after, off by one in both places. | Both occurrences corrected to SIX before / FIVE after, with the five-kind enumeration spelled out inline at the first site so the count is checkable without cross-referencing the paragraph above it. | *(this record)* |
| **P2-5** — the migrator's docstring claimed "nothing authored is dropped", but a trailing comment on a RECOGNIZED parameter line (`scale 0.0075  # hand-tuned`) was silently discarded — only the value token survived; unrecognized lines already carried their full text. | Recognized-parameter lines now carry a trailing comment too, via the same `# migrated: <raw line>` idiom (`raw != cline` after comment-stripping is the signal), counted in `stats['comments_carried']`. Extended to the `bumpmap_modifier` keyword line (with or without the brace on the same line) and the closing `}` line, both of which sit outside the interior-comment preservation's scan range. Four new selftest cases. | `cd5ae2be` |
| **P2-6** — `windowsize <= 0` makes legacy `BumpMap::Modify` INERT (central difference samples the same point on both sides; normalisation gated on `dWindow > 0`), but the migrated `step 0` means AUTO in `relief_modifier` — full perturbation, not inert — so the prior fold was lossy exactly at this boundary. | Detected as a special case: emits the bare `scale 0` (neutralises the perturbation regardless of `step`) instead of the folded algebra, with a `WARN <file>:<line>` naming the reason. Documented in §7.2. Two new selftest cases (zero and negative windowsize). | `cd5ae2be` (code + selftest), *(this record)* (§7.2 doc) |
| **P2-7** — §7.2 claimed "a `scale` that would print with more than 17 significant digits is written `%.17g`"; the migrator actually calls `repr(float(x))` unconditionally, and `%.17g` does not appear anywhere in the script. | Sentence corrected: `repr(float(x))` (David Gay's shortest round-trip decimal) is used unconditionally, not gated on a digit-count threshold. | *(this record)* |
| **P2-8** — §12's Phase 3 header said "landed … (awaiting implementation-review-loop)" while its own body said "treat it as … not LANDED"; the top status line and §11's Phase 3 row still said "awaiting review round" after this very round ran. | Header reworded to "implemented 2026-09-06 (review round 1: see fix round 1 below)"; the "NOT been through … not LANDED" paragraph rewritten to name the three reviewer lenses and point at this table; top status line and §11's Phase 3 row both updated to "Phase 3 reviewed (three lenses: code CLEAN after 1 P1 fix, teaching 3 P1s fixed, migration LOSSLESS by pixels — see §12)". | *(this record)* |
| **P2-9** — §12's Phase 3 record had no pixel-level render-fidelity evidence beyond the crackle-glaze A/B and the velvet_cushion migration check, and did not record the reviewers' further render/parse findings. | Recorded below (render evidence + pool.RISEscene note). | *(this record)* |

**Render evidence (P2-9), from the reviewers' own render passes.** Mean
linear Rec.709 luminance ratios (migrated-or-fixture vs. reference,
`oidn_denoise FALSE` throughout): `velvet_cushion` **1.0000** (block max
**1.15/255**); `sculptors_studio` **1.00006** at 1024 spp (block max
**4.08/255**); `sms_veach_egg_bumpmap` **0.9996** (block max **2.69/255**).
A fourth control render — `relief_crackle_glaze.RISEscene` with
`relief_modifier.scale` forced to `0` (i.e. the un-perturbed frame) —
differs from the authored `-0.02` render by **11.7/255** mean absolute
difference, confirming the other three ratios (all within ~0.06% of 1.0,
block-max well under a JPEG-comparable perceptual threshold) are measuring
a real lossless-migration match and not an insensitive metric that would
have passed regardless. The reviewers additionally observed that the
crackle recipe's relief legibility is **lighting-angle dependent**: strong
under raking (near-grazing) light, weak near the specular peak, where the
GGX lobe's own brightness dominates the frame and the sunken crack lines
read mostly through their colour (the ramp), not their shading. Recorded
here as an input to Phase 5's pixel-verification pass, which should include
at least one raking-light and one near-specular camera/light placement
rather than only the recipe's authored angle.

Also observed, and deliberately **not** fixed (out of scope — the file is
the user's untracked scratch scene, gitignored per `.gitignore:53`):
`scenes/Internal/pool.RISEscene` carries three PRE-EXISTING parse
diagnostics unrelated to this arc's migration, reproduced by a headless
parse (`pinhole_camera: invalid parameter(s)` — a stale `width` line from
before cameras became imaging-only per the film/camera split noted at the
top of this doc; and two `chunk braces must be on their own lines`
diagnostics, `perfectrefractor_material` and `polished_material`). None of
the three are `bumpmap_modifier`/`relief_modifier`-related; they predate
this arc and are recorded here only so a future viewer of this file's
parse log is not surprised by them.

**Gate suites, run on the final tree.** No C++ was touched in this round
(the four fixed files are two skills `.md`, one design `.md`, and
`tools/migrate_scenes_relief.py`), so no rebuild was needed.
`python3 tools/migrate_scenes_relief.py --selftest` — **18/18 ok, 0
failures** (was 12; the four P2-5 comment-carry cases and the two P2-6
non-positive-windowsize cases are the additions). `python3
tools/migrate_scenes_relief.py --dry-run -v --root scenes` — **468 files
scanned, 0 `bumpmap_modifier` chunks seen**, confirming the corpus is still
fully migrated after this round's changes. `./bin/tests/ReliefModifierTest`
— **106 passed, 0 failed**, run once per the gate (unaffected — no
executable relief-modifier C++ was touched, only the migrator script and
documentation).

### Phase 4 — implemented 2026-09-06

Branch `relief-modifier`, one commit: `88ae61e7` (`src/Library/Agent/AgentDiagnostic.h`,
`src/Library/Agent/AgentSession.cpp`, `tests/AgentReadValidateTest.cpp`).
Not yet run through the implementation-review-loop's adversarial round —
recorded here as *implemented*, matching Phase 3's own header wording
before its review landed.

**The detector (condition Q, `DESIGN_FLAT_RELIEF`).** Runs inside the
SAME shared `ComputeDesignNoteConditionsFromDoc_` scan every sibling
condition (A–P) shares, as a resolution pass right after condition P's
own (document-only, so — unlike condition M — it needs no derived scene
and runs unconditionally). Exact predicate, in the order the code checks
it: for every `standard_object`/`csg_object` in document order —

1. **Not a pure container.** A `standard_object` with neither `geometry`
   nor `source` is a transform node the renderer never draws (its own
   descriptor: "with no geometry it is a pure CONTAINER… invisible to the
   renderer itself") — nothing for a modifier to act on, so it is
   excluded regardless of what its (inert) material binds. `csg_object`
   has no `geometry`/`source` field at all (its shape is always its two
   operands) so this exclusion never applies to it. *(Added mid-round —
   see "what broke and why" below; not in the original four-clause list
   the brief specified, but required for the fixture corpus to stay
   correct.)*
2. **(ii) No modifier bound.** The object's own `modifier` parameter is
   absent, empty or `none`. A `modifier` naming a `modifier_stack` chunk
   counts as bound — the stack is itself a `ChunkCategory::Modifier`
   chunk, and this condition only asks "does the slot resolve to
   anything", never which modifier *kind*: a bumpmap/normal-map/glint
   modifier silences this exactly as a relief one would, since the claim
   is narrowly "the shading normal is inert here".
3. **(iii) Not hair geometry.** The object's geometry (resolved through
   `objectGeometryByName` → `geometryKindByName`) is not `hair_geometry`
   — a strand's own tangent-frame shading has no purchase for this kind
   of relief.
4. **(iv) Not hair/luminaire/light-object.** The bound material's kind is
   not `hair_material`; is not classified emissive by
   `DescriptorIsEmissiveMaterial_` (the registry "carries `exitance`"
   rule conditions I/M already use — reused verbatim, not re-derived);
   and the object itself does not classify as a light-object via
   `ChunkIsLightObject_` (the arc-80 rect_light/shape_light fixture
   classifier — redundant with the luminaire check on every fixture
   audited this round, kept as defense-in-depth the same way
   `TargetIsFormBearing_` keeps it).
5. **(i) A genuinely varying colour-pipe slot.** At least one slot in
   `ColorMaterialSlotsByKind_()` (condition H's own registry-derived
   table — no second slot list was written) is spelled out on the
   material AND classifies `MicrosurfaceBinding_::Varying` via
   `ClassifyColorBinding_` (condition H's own classifier). An **Opaque**
   (unreadable/unresolved) slot does **not** qualify — that proves only
   that the slot is not a plain flat constant, never that it is
   genuinely textured, the same "erring toward varying is the safe
   direction, but Opaque is not varying" reading condition H's own
   classifier already commits to.

No volume gate — condition M/N/O/I's convention (one decal-on-plastic
object already *is* the described failure), not condition D/H/L/P's
"systemic flatness across N materials" one. No hero-material pick either:
D/H/L/P each choose ONE material a bare verb call would rewrite, but
Phase 4 ships no verb, so there is nothing for a hero pick to target —
every qualifying **object** (not material) is a finding, the first named
in full and the rest counted, the `EnclosedLightFinding_`/
`DimLightFinding_` "first named, rest counted" convention (`findings[0]`
plus `"(N more objects similarly flat-shaded.)"`) rather than the
`FormatBoundedNameList_` plain-name-list convention conditions F/G/I use
— chosen because naming just the object isn't enough here; the clause
also needs the material, the slot and the painter, which only a
per-finding struct (`FlatReliefFinding_`) carries.

**Message shape.** Names the object, its material (and kind), the
varying slot, and the painter bound there; states the two-chunk fix
(`scalar_painter { painter <field> channel R }` bridging the SAME field
into `relief_modifier { height <that scalar_painter>  scale <amount> }`,
bound via the object's own `modifier`); points at
`read_skill {"name":"procedural-textures"}`'s relief section. Self-
disarming with condition H's own `kSelfDisarm` suffix on the diagnostic
carrier (the claim IS "flat/simple styling", condition A's own topic) —
the render-result note carries the shared clause without that suffix,
the same split every sibling condition with this posture (H, K) follows.
The shared clause text (not the diagnostic-only suffix) is asserted
byte-identical between `ComputeDesignNote` and the `validate` diagnostic
in the test (the "verbatim invariant" every prior condition's test
carries).

**Verb hook-point notes (assessed, not shipped).** Two comment blocks,
no behaviour change: `AgentSession::AddWear`'s rebind site (just above
where it splices the mined `expression_painter` colour field in) names
the `relief_amplitude` escalation the design's §9 describes — the SAME
noise term the rebind already mints would additionally become a
`scalar_painter` + `relief_modifier`, wrapping any existing modifier in
a `modifier_stack` (which condition Q itself already treats as "bound",
so the two mechanisms compose without a special case). `AgentSession::
AddWetness`'s coat-wrap site (just above where it mints the
`coated_material` wrapper) states the opposite: this verb must **not**
grow the same argument, because a wet film conforms to the relief
already there (its own §6.3 correction) rather than adding new
micro-geometry — minting a `relief_modifier` there would emboss the
very surface a coat is supposed to smooth. Both are pure comments; no
`add_wear`/`add_wetness` call signature or behaviour changed.

**Tests (`tests/AgentReadValidateTest.cpp`, `RunFlatReliefScanTest`).**
Six cases against a still-life vessel fixture (a lambertian ceramic
cylinder), matching this file's own non-creature convention:

- **(a) red-proof.** A varying `reflectance` (an `expression_painter`
  fbm field) and no `modifier` fires; asserts the message names the
  object, material, slot and painter, states the `scalar_painter { painter
  … channel R }` + `relief_modifier` two-chunk fix, names the attach
  mechanism, points at `read_skill` procedural-textures, and
  self-disarms. The verbatim-invariant check confirms the shared claim
  text is byte-identical between the diagnostic and `ComputeDesignNote`.
- **(b) green-proof.** The identical material/object, now with
  `modifier` naming a `relief_modifier` directly — silent.
- **(c) green-proof.** `modifier` naming a `modifier_stack` that wraps
  the SAME `relief_modifier` — silent (a stack counts as bound).
- **(d) green-proof.** A flat `uniformcolor_painter` reflectance, no
  modifier — nothing varies, silent.
- **(e) green-proof, three objects, three different reasons.** A
  `hair_material` object (material-kind exclusion, clause iv) on
  ORDINARY (non-hair) geometry; a `hair_geometry` object (geometry-kind
  exclusion, clause iii) bound to the ordinary varying material; and a
  `lambertian_luminaire_material` object with a varying `exitance`
  (luminaire exclusion, clause iv). All three silent.
- **(f) dedupe.** Two qualifying objects sharing one material still
  yield exactly ONE `DESIGN_FLAT_RELIEF` diagnostic entry (not two) —
  one finding per object internally, one diagnostic per condition
  externally, `findings[0]` named in full plus `"1 more object"`
  counted. A second `ValidateText` pass over the byte-identical document
  reproduces the SAME single entry (this scan is entirely stateless — no
  session, no cache — so there is nothing to accumulate across passes;
  "dedupe" here means "one entry per pass", not "suppressed on a second
  call").

**What broke, and why (the container-object fix).** The first build of
condition Q, run against the FULL suite, red the pre-existing
`AgentReadValidateTest` five ways: `docA3WithScalar` (a decoy object `b`
bound to a genuinely-varying `doc91_procedural_mat` reflectance, with
**no `geometry` at all** — a container used purely to keep condition H
from firing vacuously) and the shared `preamble`/`bottle`/`anchors`
fixture family used across every condition-C test (`docSix`, `docSeven`,
`docNine`, `docTen`, `docInstanced` — a `mat` material with a genuinely
varying `reflectance`, bound to real `bottle_geo` geometry across up to
ten objects, plus one `procedural_anchor` on the same geometry) all
turned out to be, by the letter of clauses (i)/(ii)/(iv), REAL
decal-on-plastic objects that simply predate condition Q. Two different
fixes, for two different reasons: `docA3WithScalar`'s `b` has no
`geometry` at all, so it is excluded outright by the container clause
above (added specifically because of this fixture — a geometry-less
decoy object is not a real surface, and firing on one would be a false
positive on ANY future document that reuses the same idiom, not just
this test). The `bottle`/`anchors`/`docB4AllBoxWithScalar` fixtures'
objects DO own real geometry, so they are genuine (if incidental)
matches for the predicate as specified — each was given a `relief_modifier`
built on the SAME field already driving its colour (never a decoy chunk),
bound via the object's own `modifier`, purely to keep the "note goes
fully empty" assertions those tests depend on. Comments at each edit site
explain why. No assertion's PASS/FAIL semantics were changed — only
enough silencing was added to keep pre-existing "should be silent"
fixtures silent under the new condition, the same maintenance every prior
condition (H, L, P) needed on the fixtures that predated it.

**Registry surfaces.** Grepped `DESIGN_DRY_RAIN_SCENE` (the newest prior
sibling) across `src/`, `tests/`, `skills/`, `docs/`: the only functional
registry is `AgentDiagnostic.h`'s own constant declaration plus the
`d.code = AgentDiagnosticCode::DESIGN_*` emission sites in
`AgentSession.cpp` — no separate DESIGN_* enumeration, JSON schema, or
`AgentMcpAdapter` listing exists to update. Both are done.

**Gate, run on the final tree.** Full library + all four CLI binaries
(`rise`, `meshconverter`, `imageconverter`, `biospecbsdfmaker`) rebuild
warning-free. `./bin/tests/AgentReadValidateTest` — **314 passed, 0
failed** (was 294 before this phase's six new cases plus the fixture
fixes above; every existing assertion in the file still passes
unchanged). `./bin/tests/AgentChunkCrudTest` — **3809 passed, 0 failed**.
`./bin/tests/SourceHygieneTest` — **164 passed, 0 failed**.
`./bin/tests/ReliefModifierTest` — **106 passed, 0 failed** (unaffected —
no `ReliefModifier.{h,cpp}` C++ was touched this phase, only
`AgentSession.cpp`/`AgentDiagnostic.h`/the one test file). Also run,
since both verbs' surrounding code (comment-only) changed:
`./bin/tests/AgentAddWearTest` — **287 passed, 0 failed**;
`./bin/tests/AgentAddWetnessTest` — **210 passed, 0 failed**.

**Self-audit — five likeliest ways this is wrong, and what was checked.**

1. **False positive on a machine-minted instancing copy.** A `count_u`
   repetition's per-copy names (`name[i,j]`) are never spelled as their
   own `standard_object` chunk — condition Q, like every sibling, walks
   literal document chunks, so it can only ever see the ONE authored
   `standard_object` that carries `source`/`count_u`, not the copies the
   derive step mints. Checked: condition C's own `RepeatGroup_`/
   `docExpressesInstancing` machinery has the identical property and
   documents it the same way; no different exposure here.
2. **False positive on a luminaire wrapping a real base material.**
   `DescriptorIsEmissiveMaterial_` fires on ANY material carrying
   `exitance`, whether or not it wraps a real base (unlike
   `ChunkIsLightObject_`, which requires the wrap to be absent). Checked
   against the descriptor registry: only `lambertian_luminaire_material`/
   `phong_luminaire_material` carry `exitance`, and BOTH are meant to be
   excluded regardless of a base wrap — micro-relief on a light's own
   emission surface is out of scope either way, so the broader exclusion
   is the intended one here (unlike condition M, which specifically needs
   the narrower "emissive-ONLY" test to find the shell BEHIND a wrapped
   luminaire).
3. **False positive on a CSG composite whose per-vertex hit carries no
   `pmxWorldToObject`** (the Phase-1 red-proof's own documented
   degraded mode for object-space fields). Not applicable here: condition
   Q is a static document scan, not a render-time evaluation — it never
   touches `RayIntersectionGeometric` or any hit record, so this
   Phase-1 concern does not transfer.
4. **Classifier reuse drift.** Checked that `ColorMaterialSlotsByKind_`/
   `ClassifyColorBinding_` are called with the EXACT same arguments
   condition H uses (the material's own already-collected param map,
   the document-wide `painterKinds` map) — no parallel copy was written,
   so a future change to either function updates both conditions
   identically by construction.
5. **Dedupe/rate-limiting beyond "one diagnostic per pass".** The task
   brief asked for the SAME rate-limiting the siblings use; conditions
   A–P carry no cooldown or session-state throttle at all (`ValidateText`
   and `ComputeDesignNote` are both fully stateless, recomputed fresh
   every call) — condition Q matches that exactly, by construction (same
   scan, same statelessness), which test (f) checks directly (two
   `ValidateText` passes over an identical document reproduce the
   identical single-entry result, proving there is no cross-call state to
   drift).

**Deviations from the brief.** (1) The container-object exclusion
(clause 1 above) was not in the brief's four numbered clauses — added
because the existing fixture corpus needed it and it is independently
correct (a geometry-less container is never a rendered surface). (2)
`AgentChunkCrudTest.cpp` does not test any DESIGN_* condition (grepped
empty for `DESIGN_UNWORN_MATERIALS` and every sibling) — the tests landed
in `tests/AgentReadValidateTest.cpp`, where `DESIGN_UNWORN_MATERIALS`,
`DESIGN_DRY_RAIN_SCENE` and every other condition in this family are
actually tested, per the brief's own "(or wherever DESIGN_UNWORN_MATERIALS
is tested — grep tests/ for it)" fallback. `AgentChunkCrudTest` is still
run and reported above (green) since the brief names it explicitly.
Nothing else was skipped: all four numbered deliverables plus this
record are complete.

### Phase 5 — landed 2026-09-06

Branch `relief-modifier`, two commits plus this record: `0f872ac9`
(weathered_workbench), `7b7b868d` (relief_crackle_glaze + golden
regen). Pixel verification per
§9's own thesis extension and §12's Phase 3 R13 finding (crackle
legibility is lighting-angle dependent). No C++ touched — scenes,
renders, and this record only, per the phase's own gate in §11.

**1. `weathered_workbench` — relief bound to the grain field.**

A third consumer of `expr_grain` was added: `scalar_painter sp_wood_relief`
bridges the field raw (channel R, no scale/bias — the amplitude lives
entirely in `relief_modifier`'s own `scale`), and `relief_modifier
relief_wood` (`domain surface`, `step 0`) binds `obj_bench_top` and all
four `obj_leg_*` objects, which all share `mat_wood_top`.

*Sign.* `ramp_wood` maps field=0 → dark heartwood, field=1 → pale
sapwood — the opposite polarity from the crackle-glaze recipe, whose
dark end is its field's HIGH end. Binding `sp_wood_relief` (monotonically
increasing with the raw field, like `ramp_wood`) with a **positive**
`scale` therefore sinks the dark grain relative to the pale wood, which
is the picture the scene wants (dense dark streaks read as the harder,
less-worn wood; the softer pale wood between them reads as
worn/weathered — this also fixed a pre-existing inverted comment on
`sp_wood_rough`, which had its own pale/dark ends backwards relative to
`ramp_wood`'s mapping). This is the opposite sign from the crackle
recipe's `-0.02`, and is called out in-scene so a future reader doesn't
assume the two recipes disagree.

*Amplitude sweep.* Reduced copies (320×240, `oidn_denoise FALSE`) at
`scale` = 0.002, 0.004, 0.008 first at 24 spp, then re-run at 128 spp
once the 24 spp renders proved too noisy to distinguish signal from
per-run Monte-Carlo seed variance (renders seed from wall clock, not a
fixed value — mean abs diff between BEFORE and a *zero-effect* AFTER at
24 spp was ~1.15–1.95/255 just from noise; at 128 spp it dropped to
0.82–1.10/255 and started tracking `scale` monotonically). The visual
effect at 0.002–0.008 was too subtle to read as relief at this table's
scale/lighting (front-lit top face, `box_geometry` — an analytic
primitive, so no distance fade either way). Extended the sweep to 0.02,
0.04, 0.05, 0.06, 0.10: 0.04–0.10 all showed a visible rippled front-edge
silhouette and catch-light along the grain ridges with no sparkle at the
tray/vise (far) end; a dark-pixel-count check in the bench-top crop
(`< 10` luminance, a proxy for normal-flip black-speckle artifacts) held
near the BEFORE baseline (37) through 0.06 (39) and only started
climbing at 0.10 (82), so 0.10 was treated as the edge of the safe range,
not the target. **Chose `scale 0.05`** — smallest value in the sweep
where the grain reads as carved surface (visible edge undulation +
catch-light) rather than flat paint, comfortably inside the
no-artifact band.

| scale | render | note |
|---|---|---|
| 0 (before) | `wb_before.png` (reduced), `wb_before_authored.png` (authored) | flat — straight top-edge silhouette, paint-like grain |
| 0.002 | `wb_after_s0002.png` | no visible difference from before |
| 0.004 | `wb_after_s0004.png` | no visible difference from before |
| 0.008 | `wb_after_s0008.png` | barely perceptible |
| 0.02 | `wb_after_s002.png` | faint edge ripple |
| 0.04 | `wb_after_s004.png` | visible edge ripple + grain catch-light |
| **0.05** | `wb_after_s005.png` (reduced), **`wb_after_authored.png`** (authored — committed) | **chosen** — reads as surface, no artifacts; NOTE the front edge of the top goes from a straight line to a visibly scalloped silhouette at this amplitude, and the supervising review judged the result a touch heavy for fine grain: the choice trades subtlety for legibility at this table scale and lighting, and `scale` is the one number to dial down (0.02–0.04 were artifact-free and quieter) |
| 0.06 | `wb_after_s006.png` | stronger, still clean |
| 0.10 | `wb_after_s010.png` | dark-pixel count roughly doubles vs. baseline — edge of the safe range |

All PNGs are under
`/private/tmp/claude-501/-Users-aravind-Working-GitHub-RISE/0c48c261-5924-45c6-a163-b53339ecf707/scratchpad/phase5/`.

*Mean luminance (authored settings, 640×480, 12 spp, `oidn_denoise` at
its scene default TRUE, 8-bit sRGB channel-averaged):* whole frame
before **150.213**, after **148.949** (−0.84%); bench-top-only crop
(rows 150–300) before **133.808**, after **130.121** (−2.76%) — both
decreases are consistent with the sunk dark grain reading darker/more
shadowed, not a variance artifact (the authored render is a single
sample count, not compared across seeds).

*Authored-settings render time:* 2.57 s wall (12 spp, 640×480) — far
under the 5-minute budget, so both the reduced-sweep evidence and a
full authored-settings AFTER render are kept
(`wb_after_authored.png`; `wb_before_authored.png` is the HEAD-scene
control at the same settings).

**2. `velvet_cushion` — migrated (`domain uv`) vs. a `domain surface` upgrade.**

The cushion's `cushion` geometry is `sdf_geometry`. Checked
`SDFGeometry.cpp` directly (the block right above the curvature
computation, ~line 1618): *"`derivatives.valid` stays FALSE — there is
no dpdu/dpdv here to be valid"* — stated in the code's own comment, not
inferred. Per design §3.2, `domain surface`'s chain-rule step for
`ptCoord` only fires `when ri.derivatives.valid`; when it is false,
`ptCoord` is left unchanged at all four offset evaluations, so a
UV-parameterised field (`crease_field`, an `expression_function2d`) reads
**flat** in `domain surface` mode on this geometry — this is the
documented mesh/SDF limitation (§3.2), not a bug.

Rendered both at reduced settings (600×450, 48 spp, `oidn_denoise
FALSE`) to confirm rather than only assert: `velvet_uv.png` (the
committed scene, unchanged) shows the fine radial nap-crease pattern
converging on the crown, matching the scene's own description; a
`domain surface` variant of the identical `relief_modifier` chunk
(`velvet_surface.png`) shows **no crease texture at all** — smooth
where the UV version shows fine radial gathers. Mean luminance is
statistically identical (77.837 vs. 77.849 — under 0.02%, i.e. no
systematic exposure difference, only the texture is different/absent),
confirming the difference is the missing relief, not a lighting or
exposure change from the domain switch.

**Verdict: migrated `domain uv` is clearly better for this scene, and
the `domain surface` upgrade is lossy (produces zero relief) — the
committed scene is left unchanged**, exactly per the task's own
condition ("do NOT change ... unless surface mode is clearly better AND
lossless in intent" — here it's neither).

**3. `relief_crackle_glaze` — legibility near the specular peak (R13/Phase 3's finding).**

Rendered the scene as authored (`crackle_baseline.png`, 256×256, 32 spp,
box filter, `oidn_denoise FALSE` — already the scene's own settings, no
reduction needed) and confirmed the finding by inspection: zoomed crops
(`crackle_baseline_zoom.png`) show crack lines visibly thinning/losing
their groove shading right where the GGX highlight sits (upper-left of
frame under the original `0.35 0.55 0.85` key direction, which is close
to the camera axis and puts the highlight near the sphere's
camera-facing centre).

Tried, in order, per the task's "ONE principled adjustment" scope:

- **Scale only** (`-0.02` → `-0.05`, light unchanged): `crackle_scale05.png`
  / `crackle_scale05_zoom.png`. Marginal improvement in the highlight
  region, not a clear fix — the highlight itself still dominates.
- **Light direction, extreme raking** (`0.90 0.25 0.05`):
  `crackle_raking.png` / `crackle_raking_zoom_center.png` /
  `crackle_raking_zoom_highlight.png`. Cracks read clearly near the
  (now off-centre) highlight, but roughly half the sphere fell into
  near-black under only the 0.12-power ambient fill — worse legibility,
  just relocated.
- **Light direction, moderate raking** (`0.72 0.42 0.35`, **chosen**):
  `crackle_mid.png` / `crackle_mid_zoom_center.png` /
  `crackle_mid_zoom_highlight.png`. The highlight moves off the
  sphere's camera-facing centre toward the upper-right limb; crack
  grooves read by shading (catch-light on one rim, shadow on the other)
  across the whole visible hemisphere, including near the (now
  off-centre, less dominant) highlight, while the sphere stays
  reasonably front-lit overall.

Committed the light-direction change only (`7b7b868d`); scale stays
`-0.02`. Confirmed with a final render of the actual committed scene at
its authored settings, matching `crackle_mid.png` (copy kept as
`crackle_final.png`). Mean luminance (authored settings, 256×256, 32
spp, `oidn_denoise FALSE`): baseline **66.002**, final **48.233** — the
frame is dimmer because the highlight (the single brightest region) has
moved away from the camera-facing centre onto the limb, which is
expected and is exactly the intended trade (peak brightness for crack
legibility).

**Skill-excerpt invariant.** Only the scene's `directional_light` chunk
changed; that chunk was never part of the `materials-and-media-basics.md`
excerpt (which reproduces only `cg_crack_field`, `cg_glaze_colour`,
`cg_crack_scalar`, `cg_relief`, `cg_f0`, `cg_glaze_mat`, and the
`standard_object`). Re-diffed all six painter/material chunks plus the
object chunk against the excerpt after the edit: all six chunks (the
object chunk carries a deliberate, pre-existing difference — no
`position`, a `# any sphere_geometry / SDF / mesh works` comment — that
predates this phase and is not part of the "must match" set, matching
what the Phase 3 P1-1 fix already established) remain byte-identical.
No skill edit was needed or made.

**Golden regen.** `./bin/tests/CstDeriveGoldenTest --generate` picked up
both Phase 5 scene edits correctly, but also picked up the pre-existing
*uncommitted* drift on `scenes/FeatureBased/BDPT/bdpt_crystal_garden.RISEscene`
(the stray edit named in §11 as predating this arc and left alone) —
generate re-derives whatever is on disk, dirty or not, so its digest
moved too. That one line was hand-reverted back to its HEAD value in
`tests/data/cst_derive_golden.txt` before committing, so the golden
file's only changes are the two Phase 5 scenes'
(`weathered_workbench.RISEscene`, `relief_crackle_glaze.RISEscene`).
Verified with `./bin/tests/CstDeriveGoldenTest` (verify mode, not
generate): **440 MATCH, 1 DRIFT** (that same pre-existing
`bdpt_crystal_garden` entry — unchanged from before this phase started,
confirmed by re-checking it reproduces the untouched HEAD digest), **0
UNCOVERED, 0 STALE**.

**Left undone / observed-need gated.** Nothing from the four deliverables
was skipped. Not attempted (out of scope for this phase): re-tuning
`relief_crackle_glaze`'s `scale` in combination with the light change
(the task asked for ONE principled adjustment; the light-only change
already closed the finding without also touching the recipe's
`-0.02`/`0.46`/`0.04` values that the skill excerpt and §9's prose
describe). The `weathered_workbench` legs share `mat_wood_top`, so they
were bound to `relief_wood` alongside the top rather than left flat —
the scene's own header never called the legs out separately, and
leaving them flat while the top gained visible relief would have read
as an inconsistency in the same material.

---

### Phase 4 — fix round 1 (2026-09-06)

One adversarial review round on the Phase-4 record above returned **3 P1s
+ 3 P2s**. All six fixed at the root in one commit, `177bcee4`
(`src/Library/Agent/AgentSession.cpp`,
`tests/AgentReadValidateTest.cpp`, `tools/migrate_scenes_relief.py`).

**The common root of P1-1 and P1-2.** Condition Q's clause (ii) asked
whether the object's own chunk spells a `modifier` param. That is not the
same question as *does the rendered surface carry a modifier* — the
engine answers the second one through two mechanisms the literal-param
test cannot see. One helper now resolves it:
`ObjectHasEffectiveModifier_` (the brief's `EffectiveModifierOfObject_`;
it returns a **bool** rather than a name because the "every operand binds
one" arm has no single modifier to name), off an
`EffectiveModifierIndex_` the existing single document walk fills in.
**The rules it implements**, in the order it applies them:

1. **Own, or inherited down the `source` chain.** Walk `source` until a
   level SPELLS `modifier`; that level's value is the answer.
   `MergeChunkParams` ([Cst.cpp](../src/Library/Cst/Cst.cpp)) folds the
   whole chain into the derived instance and `modifier` is **not** in
   `IsInstanceOwnParam` (only `name` / `parent` / `source` / the five
   transform params / `mirror` / `count_u` / `count_v` are), so an
   instancing copy renders with its source's modifier. **`none` is a
   first-class, chain-STOPPING answer**: the copy's own params merge
   LAST and the parser passes 0 for a `"none"` modifier (Cst.cpp's own
   optional-slot-removal rule), so `modifier none` genuinely CLEARS an
   inherited binding. This is why the collected map records `"none"`
   where every sibling map drops it.
2. **A composite whose EVERY operand is bound.**
   `CSGObject::IntersectRay` reports the OPERAND's modifier on each hit
   (`AdoptCsgSurfaceBindings`), so a `csg_object` binding nothing itself
   still has a fully relief-bearing surface when both operands do —
   recursively, through nested csg.
3. **An enclosing composite that binds one.** The composite's own
   binding takes final precedence over the operand's (the conditional
   overrides at the bottom of `IntersectRay`), so an operand under a
   relief-bearing composite is never flat. Walked transitively.
4. **Otherwise: unbound.** A composite with no operands recorded (an
   unresolved `obja`/`objb`) is NOT bound — this scan never claims
   relief it cannot see. **A composite with no modifier and only SOME of
   its operands bearing relief therefore FIRES, naming the composite**
   (rule 2 fails; the partly-flat surface is exactly the described
   failure).

All three walks are depth-bounded at 8, so a `source` cycle or
mutually-referencing composites in a malformed document resolve to "no
modifier" — which costs at most one advisory — instead of hanging the
scan. *(Superseded in part: fix round 2 below raised the `source` bound
to the engine's own 256 — the cycle this 8 was justified by cannot
happen, and 8 made a legal 8-deep chain fire falsely. The two csg
walks keep a nesting bound of 8.)*

**P1-3 — the quadratic scan.** `FindDocumentChunkByName_` re-walked the
whole document per call, and condition Q called it once per candidate
**ahead of clause (i)**, with two more walks inside
`ChunkIsLightObject_`. It is replaced by `DocChunksByName_` — every
chunk bucketed by `name`, in the same `CollectItems` order, first chunk
of the required category wins, so **no migrated caller can resolve a name
to a different chunk**. Built ONCE and LAZILY (only the light-object test
needs it), with that test moved AFTER clause (i) so a non-candidate never
pays for it. The old helper had no remaining callers and was deleted;
its per-category-uniqueness doc moved onto `FindIndexedChunkByName_`.

Measured with a scratch harness against `bin/librise.a`
(`ValidateText` + `ComputeDesignNote` over a generated document of N
`sphere_geometry` + `lambertian_material` + `standard_object` triples,
none binding a modifier; times are seconds, macOS arm64, `-O3`):

| N | before, no-fire | after, no-fire | before, all-fire | after, all-fire |
|---:|---:|---:|---:|---:|
| 500  | 0.479 / 0.438 | 0.035 / 0.018 | 0.456 / 0.436 | 0.016 / 0.014 |
| 1000 | 1.966 / 1.948 | 0.036 / 0.030 | 2.005 / 2.024 | 0.033 / 0.030 |
| 3000 | 19.079 / 19.073 | 0.108 / 0.102 | 19.434 / 19.360 | 0.108 / 0.099 |

(each cell is `ValidateText / ComputeDesignNote`; "no-fire" binds a
`uniformcolor_painter` reflectance so no object qualifies, "all-fire"
binds the fbm field so every object reaches the light-object test.)
Before: ×4.1 for ×2 objects and ×9.7 for ×3 — clean quadratic. After:
×3.1 for ×6 objects — linear, and **0.108 s at N=3000**. **Honest note
on the "before" column:** it was produced by a temporary reconstruction
of the pre-fix shape (a per-candidate index build ahead of clause (i)),
not by the deleted code byte-for-byte, so the absolute constant is
~2× the reviewer's own 8.6 s at N=3000. The *growth exponent* — the
thing being fixed — is the same measurement either way.

**Audit-by-bug-pattern sweep.** Grepped every `FindDocumentChunkByName_`
call site for the same "helper inside a per-item loop" shape. Conditions
H / I / M do **not** have it (they resolve names off the maps the shared
walk already built, never by document lookup). One sibling did:
`ClassifyDocumentChunkForFormBearing_`'s referencer loop called
`ChunkIsLightObject_( r, doc )` — two whole-document walks — once per
referencing chunk. It now builds and shares one index for the loop.

**P2-1 — structurally constant "varying" painters.**
`ClassifyColorBinding_` was purely name-list based, so a `blend_painter`
whose `colora`, `colorb` and `mask` are all `uniformcolor_painter`
chunks classified **Varying** — and Q advised relief on a single flat
colour. It now recurses through the **PASS-THROUGH** colour painters,
exactly as the scalar twin `ClassifyMicrosurfaceBinding_` already walked
its `base`/`multiply` chains: Constant iff every spelled input is,
Varying if any input is, Opaque otherwise (an unresolved input still
declines rather than being guessed). The pass-through set is
`blend_painter` (colora/colorb/mask), `ramp_painter` (`input` — the
descriptor's spelling, **not** `source`), `mapping_painter` (`source`)
and `channel_painter` (`source`). It is a LIST, not "every Painter-kind
Reference param", because `checker_painter` / `perlin3d_painter` /
`voronoi3d_painter` also name `colora`/`colorb` but generate their OWN
spatial pattern between them — two uniform inputs still give a
chequerboard. `composite_function2d_painter` is deliberately absent (its
field is the Function2D pipe, which this colour classifier does not
read), so it classifies exactly as before. Condition H's semantics are
unchanged in shape — non-Constant still declines — and its own cases all
still pass.

**P2-2 — wrappers and the wetness collision.** (a) `coated_material` /
`fabric_material` / `composite_material` wrapping a varying base were
invisible to Q: the wrapper has no colour slot of its own that varies
while the surface under it is fully textured. The slot search is now
recursive (depth-bounded at 4) through every `ParameterPipe::Material`
Reference param — **registry-derived** via a new
`MaterialWrapperSlotsByKind_`, never a hand list, so a wrapper kind added
later is covered with no edit — and it applies the material-KIND
exclusions (hair, luminaire) at **every** level, so a coat over a
luminaire base is still excluded. The finding names the wrapped material
that actually carries the varying slot. (b) `AddWetness`'s GGX/PBR branch
rebinds the colour slot to a wetness-prelude `expression_painter`; Q then
fired and advised relief on a wet film, which that verb's own hook-point
note calls physically backwards ("a wet film CONFORMS to the surface
underneath it… minting a `relief_modifier` here would emboss the very
surface a coat is supposed to smooth"). Condition H's own
`WetnessBodyReadsPreludeDefs_` marker (`dryness` **and** `film_amount`)
is reused verbatim to disqualify such a binding — no second name check.

**P2-3 — varying `emissive` on a non-luminaire kind.** `ggx_material`
and `pbr_metallic_roughness_material` both carry an optional `emissive`
painter, which `ColorMaterialSlotsByKind_` rightly lists (it is
registry-derived off `ParameterPipe::Color`) — but relief cannot sell a
GLOW. `ColorSlotIsEmissionRole_` excludes `emissive` / `exitance` /
`emission` from the search. The `exitance`-descriptor material-kind
exclusion is unchanged and still applies first; listing `exitance` here
too is belt-and-braces for a luminaire reached as the BASE of a wrapper.
The exclusion is **slot-scoped, not material-scoped**: the same material
with a varying `rd` still fires.

**Phase 3 residual P2 — the migrator's line endings.**
`tools/migrate_scenes_relief.py` used `Path.read_text()` / `write_text()`,
which open in universal-newline mode: a CRLF scene came back LF, rewriting
every line in the file including the ones the migrator never touched. Both
ends now go through `read_preserving_newlines` /
`write_preserving_newlines` (`newline=''`, CRLF re-applied on write, so
emitted chunks match the file's own convention too). **Honest bound,
stated in the docstring:** only a UNIFORM-CRLF file round-trips exactly;
a mixed-ending file is still normalised to LF — unchanged from before, and
deliberately, because this migrator inserts and removes lines, so "line
k's original terminator" has no well-defined answer once the indices
shift.

**Tests.** Nine new cases appended to `RunFlatReliefScanTest`
(`tests/AgentReadValidateTest.cpp`), (g)–(o): (g) a `source` copy
overriding only `material` is silent; (h) the same copy spelling
`modifier none` fires, naming `copy`; (i) a composite overriding only
`material` with both operands bearing relief is silent; (j) a composite
carrying the relief over varying-material operands is silent on the
operands; (k) a composite with no modifier and only ONE operand bearing
relief fires, naming the composite, and is the ONLY finding; (l) an
all-uniform `blend_painter` is silent and the same blend with one varying
input fires; (m) a `coated_material` over a varying base fires, naming
the object and the wrapped material; (n) a wetness-prelude rebind is
silent; (o) a varying `emissive` over flat rd/rs is silent while a
varying `rd` on the same material still fires. Three new CRLF cases in
the migrator's `--selftest`.

**Red-proofs.** Reverting clause (ii) to the literal-param lookup and
restoring the per-candidate document walk (temporary patch, reverted)
turns **(g), (i) and (j) RED** — `AgentReadValidateTest` 323 passed / 3
failed — while (h) and (k) stay green, which is correct: those are the
cases that fire under both shapes. The P1-3 timing table above is the
same run's before-column.

**Gate, on the final tree.** Full library + all four CLI binaries rebuild
warning-free after touching every changed `.cpp`.
`./bin/tests/AgentReadValidateTest` — **326 passed, 0 failed** (was 314).
`./bin/tests/AgentChunkCrudTest` — **3809 passed, 0 failed**.
`./bin/tests/AgentAddWearTest` — **287 passed, 0 failed**.
`./bin/tests/AgentAddWetnessTest` — **210 passed, 0 failed**.
`./bin/tests/SourceHygieneTest` — **164 passed, 0 failed**.
`./bin/tests/ReliefModifierTest` — **106 passed, 0 failed**.
`python3 tools/migrate_scenes_relief.py --selftest` — 0 failures;
`--dry-run --root scenes` — 468 files scanned, 0 chunks.

**Left undone.** Nothing from the six findings. Not attempted: the
`--selftest` CRLF cases cover uniform CRLF and uniform LF only; a
per-line-terminator-preserving migrator (the mixed-ending case) is
declined above with its reason. No `.RISEscene` in the corpus is CRLF
today, so the fix is a guard for foreign input rather than a corpus
repair.

### Phase 4 — fix round 2 (2026-09-06)

A fresh adversarial round on the fix-round-1 tree returned **1 P1 + 2 P2s
+ 1 nit**. All four fixed at the root in one commit, `e5d0fa0b`
(`src/Library/Agent/AgentDiagnostic.h`,
`src/Library/Agent/AgentSession.cpp`,
`tests/AgentReadValidateTest.cpp`).

**P1 — clause (i) ignored the composite's MATERIAL override.** Fix round
1 read `CSGObject::IntersectRay` correctly for the *modifier* and stopped
there. The composite applies its own bindings through a **matched pair**
— `if( pMaterial ) ri.pMaterial = pMaterial;  if( pModifier )
ri.pModifier = pModifier;` — over an operand hit that
`AdoptCsgSurfaceBindings` has already copied *both* of. So the two engine
rules fix round 1 taught clause (ii) are rules about the pair, and clause
(i) was still resolving the operand's own `material` literal. Failing
scenario: `op_a` binds a varying material and no modifier, and its
enclosing `comp` spells `material flat_mat` — Q fired on `op_a`,
advising relief for a texture that is never shaded.

The fix is symmetric with the enclosing-composite modifier rule and runs
on the same index edges: `EnclosingCompositeBindsMaterial_`, the material
twin of `EnclosingCompositeBindsModifier_`. An object whose enclosing
chain reaches a composite that spells a `material` is not a candidate on
its own material; the **composite** is the candidate, on the material it
spells, which the ordinary per-object walk already reaches. Nested: any
enclosing composite spelling one silences the operand, because the
outermost spelled material is what survives the inside-out adoption.
One asymmetry with the modifier rule, stated in the code: `material none`
on a COMPOSITE overrides nothing (the parser passes 0, `pMaterial` is
null, the guarded assignment never fires), whereas `modifier none` on a
`source` copy genuinely clears — so "none" is chain-stopping in the
`source` walk and simply unbound in the composite rule.

**Why silencing an operand is safe** (now stated in all three places — the
`DESIGN_FLAT_RELIEF` doc comment, `EffectiveModifierIndex_`'s block
comment, and §9): **a CSG operand never renders standalone.**
`CSGObject::AssignObjects` marks both operands consumed
(`Object::AddConsumer`; `IsWorldVisible()` is `bIsWorldVisible &&
nConsumedBy == 0`), which takes them out of every world-visible
enumeration, and `Job::AddCSGObject` refuses an operand that is
`parent`ed elsewhere ("Parent the csg_object instead"). The only surface
an operand contributes to is its composite's — which is what makes both
enclosing-composite rules a narrowing of the candidate set rather than a
loss of coverage.

**P2-1 — the 8-hop `source` cap.** `for( int hop = 0; hop < 8 …)` reaches
only 7 hops, and its comment justified the bound by a `source` cycle that
`Cst.cpp` makes impossible: forward references are refused by the
declare-earlier rule, and `SourceChainOf` caps at 256 purely as a belt
against a future apply path. A **legal** 8-deep chain with the modifier
at its root therefore resolved to "no modifier" at its far end and fired
falsely **when the far link also re-spelled its own material** —
without that, the far link has no material at all and the object is
dropped as a candidate rather than fired on (reviewer-measured: 7 hops
silent, 8 fires). The bound is now `kSourceChainHopBound_ = 256` —
matched to `SourceChainOf` rather than shared, because that function is
file-static in `Cst.cpp`. The walk this bound guards actually REACHES
255 hops (hop 0 resolves the object's own literal, hops 1..255 walk its
`source` ancestors), and that reach is itself a belt that never binds:
the engine's real instancing-expansion cap is `ClonePlanBuilder::
DepthOk` (`Cst.cpp`), which refuses to EXPAND a `source` chain past 64
levels of instancing — comfortably inside the 255 this scan reaches.
`SourceChainOf`'s own 256 is a second, looser belt against a future
apply path that admits a longer chain than `DepthOk` allows today; the
point is that a chain the *engine* accepts and expands is a chain this
scan resolves, and one the engine refuses is one no derived scene
contains. The comment now says what the bound actually does. Per
audit-by-bug-pattern, the two sibling `source` walks in the same file
carried the identical shape and are fixed the same way: condition L's
`geometryKindOfObject` and condition P's `geometryKindOfObjectForWetness`
(both resolved a 9-deep instancing chain to "no geometry", dropping the
object from those conditions' evidence). The **csg** walks keep a
deliberately modest nesting bound of 8 — a different bound for a
different hazard, since `enclosingCsgByObject` can fan out — and that
distinction is now written down rather than lumped into "all three walks
are depth-bounded".

**P2-2 — clause (i) was not `source`-aware.** `objectMaterialByName`
drops objects that spell no `material`, so `copy { source orig }`
inheriting a varying material with no modifier was never a candidate —
an under-report, and one that also left the clause's "and N more objects"
tally short. The material is now resolved through the **same** chain walk
the modifier uses (`ResolveInheritedLiteral_`, which both rules share:
own literal wins, else the inherited one, `none` handed back to the
caller to interpret), via `ObjectOwnOrInheritedMaterial_`.

**Nit.** The comment above condition Q's resolution pass pointed at
`objectModifierByName`, a map fix round 1 deleted; it now points at
`effectiveModifiers` and at `EffectiveModifierIndex_`'s block comment.

**Tests.** Five new cases appended to `RunFlatReliefScanTest`
(`tests/AgentReadValidateTest.cpp`), (p)–(r): **(p)** an operand under a
composite that spells its own `material` is silent; **(p')** the
identical document with that one line dropped fires, naming `op_a` (the
control that makes (p) a rule about the override, not a blanket "operands
never fire"); **(p'')** nested composites where the MIDDLE one overrides
the material and the outer does not — both operands silent, the middle
composite the only finding; **(q)** `orig` + a bare `copy { source orig }`
both count, the first named in full and the copy tallied as "1 more
object"; **(r)** a legal 9-deep `source` chain with the relief at its
root is silent at every link.

**Red-proof.** Commenting out the new
`EnclosingCompositeBindsMaterial_` call (temporary patch, reverted) turns
**(p) and (p'') RED** — `AgentReadValidateTest` 330 passed / 3 failed
(both (p'') assertions fail: the operands become findings, so the middle
composite is no longer the only one). (p'), (q) and (r) stay green, which
is correct — none of them depends on that predicate.

**Gate, on the final tree.** Full library + all four CLI binaries rebuild
warning-free after touching every changed `.cpp`.
`./bin/tests/AgentReadValidateTest` — **333 passed, 0 failed** (was 326).
`./bin/tests/AgentChunkCrudTest` — **3809 passed, 0 failed**.
`./bin/tests/AgentAddWearTest` — **287 passed, 0 failed**.
`./bin/tests/AgentAddWetnessTest` — **210 passed, 0 failed**.
`./bin/tests/SourceHygieneTest` — **164 passed, 0 failed**.
`./bin/tests/ReliefModifierTest` — **106 passed, 0 failed**.

**Left undone.** Nothing from the four findings. Not attempted: the csg
nesting bound stays at 8 rather than following the `source` walks to 256
— the enclosure edges fan out, so a large bound trades one bounded
false-advisory for an unbounded walk, and no fixture or corpus scene
nests composites anywhere near that deep. Deliberately out of scope: the
census remains user-run (C-MEAS), unchanged from fix round 1.
