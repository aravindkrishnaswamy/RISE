# Relief Modifier — Painter-Driven Shading-Normal Micro-Relief, and the Deprecation of `bumpmap_modifier`

**Status:** DESIGN (2026-09-05). Phases 1–5 below are the implementation plan;
each phase runs the [implementation-review-loop](skills/implementation-review-loop.md)
to zero P1 before the next starts. The per-phase record is appended to §12 as
phases land.
**Branch:** `relief-modifier` off `master` at `2cf923b7`.
**Inputs (verified in tree, 2026-09-05):** the three existing modifiers
([BumpMap.cpp](../src/Library/Modifiers/BumpMap.cpp),
[NormalMap.cpp](../src/Library/Modifiers/NormalMap.cpp),
[GlintModifier.cpp](../src/Library/Modifiers/GlintModifier.cpp)) and their
descriptors ([ChunkParserRegistry.cpp:8534-8690](../src/Library/Parsers/ChunkParserRegistry.cpp));
the hit record ([RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h));
the modifier hook and world-space promotion in
[Object.cpp:658-977](../src/Library/Objects/Object.cpp); the physical-scalar pipe
([IScalarPainter.h](../src/Library/Interfaces/IScalarPainter.h),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md)) and its resolver
`ResolveOrDiagnoseScalar` ([Job.cpp:3869-3928](../src/Library/Job.cpp)); the
expression VM context ([ExpressionEval.h:681-689](../src/Library/Painters/ExpressionEval.h),
[ExpressionPainter.cpp:101-119](../src/Library/Painters/ExpressionPainter.cpp));
the filter-width plumbing ([TextureFootprintCompute.h](../src/Library/Intersection/TextureFootprintCompute.h),
[ProceduralNoiseCore.cpp:62-220](../src/Library/Utilities/ProceduralNoiseCore.cpp));
the copy-then-offset precedent in [MappingPainter.cpp:96-125](../src/Library/Painters/MappingPainter.cpp);
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
[Painter.cpp:54-71](../src/Library/Painters/Painter.cpp)), and
`normal_map_modifier`, which decodes an image. So the grain field that
darkens and roughens the workbench top cannot also tilt its normal, and
authored variation reads as paint on plastic. The flagship in-tree bump user
says exactly this about itself: *"Under directional lights alone the cushion
renders as dark glossy plastic — the look this material exists to replace"*
([CLOTH_FABRIC_DESIGN.md:3673](CLOTH_FABRIC_DESIGN.md)).

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
   through `pFunc2DManager` ([Job.cpp:6647](../src/Library/Job.cpp)) and
   evaluated as `displacement.Evaluate(u, v)` per vertex
   ([GeometryUtilities.cpp:500-530](../src/Library/Geometry/GeometryUtilities.cpp)).
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
   relief recipe routes agents to `displaced_geometry`,
   [procedural-textures.md:51](../skills/agent/procedural-textures.md)).
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
| Hook timing | `Object::IntersectRay` promotes `vNormal` and `vGeomNormal` to world space, builds `onb` (from a geometry-supplied tangent via `CreateFromWU` when `bHasShadingTangent`, else `CreateFromW`), fills `derivatives`, `txFootprint`, `signals`, then assigns `ri.pModifier`. `Modify` fires at every *consumer* (RayCaster, PT, BDPT eye/light walks, photon tracers, SMS, SSS, AOVs — ~25 sites) immediately after the cast, before any material call. | [Object.cpp:658-977](../src/Library/Objects/Object.cpp), [RayCaster.cpp:1271-1277](../src/Library/Rendering/RayCaster.cpp) |
| Geometric normal | captured before the modifier and never touched by it; every SPF/BRDF's geometric-horizon gate compares against it with the `SquaredModulus > 1e-12` degeneracy guard | [RayIntersectionGeometric.h:153-176](../src/Library/Intersection/RayIntersectionGeometric.h), [GGXSPF.cpp:157](../src/Library/Materials/GGXSPF.cpp), [DielectricSPF.cpp:153](../src/Library/Materials/DielectricSPF.cpp) |
| BDPT / VCM | `Modify` runs once per surface vertex, then `normal`, `geomNormal`, `onb` are frozen into `BDPTVertex`; VCM reuses that record via `PopulateRIGFromVertex`. No integrator has a bump-terminator correction; none needs a change for a new modifier. | [BDPTIntegrator.cpp:2103-2114](../src/Library/Shaders/BDPTIntegrator.cpp), [BDPTVertex.h:102-110](../src/Library/Shaders/BDPTVertex.h) |
| Frame rebuild | all three modifiers project the *current* `onb.u()` onto the new normal's plane, `CreateFromWU`, and restore incoming handedness with `FlipV` (the mirrored-instance fix); fall back to `CreateFromW` if the projection degenerates | [NormalMap.cpp:220-234](../src/Library/Modifiers/NormalMap.cpp), [GlintModifier.cpp:252-269](../src/Library/Modifiers/GlintModifier.cpp) |
| One modifier per object | `Object::pModifier` is a single pointer; `AssignModifier` replaces; a CSG composite's own modifier *overrides* the child's. A 2002 comment says "this should be a list of some sort... eventually". | [Object.h:37](../src/Library/Objects/Object.h), [CSGObject.cpp:1612](../src/Library/Objects/CSGObject.cpp), [RayIntersection.h:35](../src/Library/Intersection/RayIntersection.h) |
| Scalar pipe | `IScalarPainter::GetValuesAt(ri)` → `ScalarTriple`; single-scalar slots read `.v[0]` and the resolver rejects per-channel painters when `requireSingle`; an `IPainter` name bound to a scalar slot gets `kScalarBoundToIPainterFmt` | [IScalarPainter.h:113-153](../src/Library/Interfaces/IScalarPainter.h), [Job.cpp:3869-3928](../src/Library/Job.cpp), [ChunkDescriptor.h:64-72](../src/Library/Parsers/ChunkDescriptor.h) |
| Any-painter bridge | `scalar_painter { painter X channel R\|G\|B\|A [scale] [bias] }` → `PainterChannelScalarPainter`; `scalar_painter { function2d F }` → `Function2DScalarPainter` (evaluates `F.Evaluate(ptCoord)` — the *same* sampling path `bumpmap_modifier` uses today) | [ChunkParserRegistry.cpp:1589-1606](../src/Library/Parsers/ChunkParserRegistry.cpp) |
| Filter width | `ri.txFootprint.worldWidth` (Igehy ray differentials), populated by triangle-mesh geometry only; `fw = 0` on analytic primitives. The expression VM's `perlin/fbm/turbulence/ridged` fade octaves against it (smoothstep, resolved below 0.2, faded at 0.6; abs-based noises fade to their measured mean) | [TextureFootprintCompute.h:48-164](../src/Library/Intersection/TextureFootprintCompute.h), [ProceduralNoiseCore.cpp:73-79,196-220](../src/Library/Utilities/ProceduralNoiseCore.cpp) |
| Evaluate-elsewhere idiom | `RayIntersectionGeometric ri2 = ri; ri2.ptIntersection = ...; source.GetColor(ri2)` — `MappingPainter` does exactly this for world/object/UV remaps and invalidates `txFootprint` only when the UV *domain* is remapped | [MappingPainter.cpp:96-125](../src/Library/Painters/MappingPainter.cpp) |
| Space semantics | `Proj_World` reads `ptIntersection`; `Proj_Object` and `voronoi3d space object` read `ptObjIntersec`; UV painters read `ptCoord`; triplanar reads `ptIntersection` + `vNormal` | [MappingPainter.h:92-189](../src/Library/Painters/MappingPainter.h) |
| Deprecation conventions | no `ChunkDescriptor::deprecated` field exists. Three precedents: (a) *removed* → generic `kUndeclaredParameterFmt` / unknown-chunk hard fail (`branching_threshold`, BDPT `sms_*`); (b) *accepted-and-ignored* → declared with `"Legacy — ignored"` (`branch`); (c) *deprecated-with-prose* → description prefixed `"DEPRECATED (...)"` (`lights_intensity_override`). Description text flows verbatim into the agent tool schema (`SchemaGen.cpp`) and the GUI suggestion surfaces. | [ChunkParserRegistry.cpp:5938,10393-10400,11466-11472](../src/Library/Parsers/ChunkParserRegistry.cpp) |
| ABI freeze | `IJob::AddBumpMapModifier(name, func, scale, window)` is signature-frozen; the sole out-of-tree caller is [rise_blender_bridge.cpp:992](../src/Blender/native/rise_blender_bridge.cpp) | [IJob.h:1626](../src/Library/Interfaces/IJob.h) |
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

**Frame rebuild** is the shared block verbatim from `NormalMap.cpp:220-234`
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

### 3.4 One field on the hit record

`RayIntersectionGeometric` gains

```cpp
const Matrix4* pmxWorldToObject;   // linear map for tangent steps; nullptr when unknown
```

defaulted `nullptr`, stamped by `Object::IntersectRay` beside `ri.pModifier`
(and by `CSGObject::IntersectRay` with the composite's own inverse when it
overrides the child's modifier — the implementer verifies which matrix maps
the world step onto the `ptObjIntersec` the child painters will read, and
adds a test that a rotated-instance object-space height field gives the same
`N'` as the equivalent world-space field). When the pointer is null the
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

`modifier` is a repeatable `Reference` on `{Modifier}` (precedent:
`standard_shader`'s repeatable `shaderop`,
[ChunkParserRegistry.cpp:10918](../src/Library/Parsers/ChunkParserRegistry.cpp)).
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
  Fresnel cosine the shading normal ([PathTracingIntegrator.cpp:2439-2461](../src/Library/Shaders/PathTracingIntegrator.cpp)); unchanged.

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
bounds at `1e-12`. The object's `modifier N` line needs no edit. A `scale`
that would print with more than 17 significant digits is written `%.17g`.

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
| `tests/data/cst_derive_golden.txt` | regenerate **after `git add`** of every new scene (the ls-files lesson); expected diff = the migrated entries' digests + additions-only for new scenes; zero drift elsewhere |

### 7.4 What else consumes `expression_function2d` afterwards

`displaced_geometry.displacement` (10 scenes), `function2d_painter` (4),
`scalar_painter { function2d }` (1, `watch_dial`), `composite_function2d_painter`,
`sdf_geometry.heightfield_function`. The frozen UV-only contract stays
frozen ([GEOMETRY_SHADING_SIGNALS_DESIGN.md:1668](GEOMETRY_SHADING_SIGNALS_DESIGN.md)).
Its teaching line in `procedural-textures.md:51` is rewritten to name
**displacement** as its niche and to stop implying any normal-perturbation
use. The legacy surface shrinks by one consumer; the remaining ones are all
vertex-time or explicit-bridge uses, which is the right shape for a frozen
evaluator.

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
tolls (§7 decision 2, reaffirmed in [WETNESS_COAT_DESIGN.md:2118](WETNESS_COAT_DESIGN.md)).

- **Recipe.** One worked example in `materials-and-media-basics.md`: a
  crackle-glaze ceramic where **one** `expression_painter` cell field drives
  colour (ramp), roughness (`scalar_painter { painter … channel R }`) and
  relief (`relief_modifier` on the same bridge, negative scale so cracks
  sink). Execution-validated (it is the §10 fixture). `procedural-textures.md`
  gets the reference row and the hook-line rewrite; everything else gets a
  pointer.
- **Advisory `DESIGN_FLAT_RELIEF`** (the decal-on-plastic detector): fires
  when an object's material has a spatially-varying painter on its colour
  slot (any non-uniform painter kind) *and* the object binds no modifier;
  names the object, the field, and the two-chunk fix. Advisory-only. Its
  adoption is expected to be ≈ 0 per C-ADV; it exists to make the census
  (below) measurable, and its text is the one place the pattern is taught
  at the moment the agent is looking at the material.
- **Verb hook points** (assessed, not shipped): `add_wear` already mints a
  `curv`-driven `expression_painter` and rebinds colour and roughness
  (`AgentSession.cpp:37034-37078`); relief cannot come from `curv` (§3.2 —
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

Four extra height evaluations per hit on objects that bind relief; nothing
on objects that do not. For an `fbm`-class expression height that is ~4× the
cost of one albedo evaluation on the same object, per hit, measured on the
§8 fixture and recorded in §12 (the standing cost-honesty lens). The dual-
number VM (§6) is the named refinement if it matters; it does not for the
showcase fixtures at their authored spp.

---

## 11. Phases

| Phase | Deliverable | Gate |
|---|---|---|
| **1** | `ReliefModifier` + `ModifierFrame.h` hoist + `pmxWorldToObject` + API/IJob/parser + 5 build projects + `ReliefModifierTest` 1–8, 10 + `cc_relief_modifier` + `relief_sphere_no_uv` | zero-P1 round; PT/BDPT parity on the sphere |
| **2** | `modifier_stack` + test 9 + `cc_modifier_stack` + §4 order doc in the descriptor | zero-P1 round |
| **3** | deprecation diagnostic + migrator + migrate 4 scenes + CST twins + golden regen + teaching surfaces (skills, `Parsers/README.md`, `GLTF_IMPORT.md` living text, `MATERIALS.md`, descriptor text) + §7.4 audit note | zero-P1 round; golden additions-only beyond migrated entries |
| **4** | `DESIGN_FLAT_RELIEF` + recipe example + hook-point notes | zero-P1 round; `AgentChunkCrudTest` green |
| **5** | pixel verification: `weathered_workbench` before/after with relief bound to `expr_grain` (kept in the showcase), `velvet_cushion` migrated vs. `domain surface` upgrade; look, and record | renders attached to §12; the user judges "reads as surface" |

Commits as each phase converges; never push. The stray uncommitted edit to
`scenes/FeatureBased/BDPT/bdpt_crystal_garden.RISEscene` predates this arc
and is left alone.

---

## 12. Phase record

*(appended as phases land)*

### Phase 1 — landed 2026-09-05

Branch `relief-modifier`, four commits off `563204b8`:

| Commit | What |
|---|---|
| `9b4f29fb` | `ModifierFrame.h` hoist + `pmxWorldToObject` on the hit record |
| `3deff3e2` | `ReliefModifier.{h,cpp}` + API/IJob/Job/parser + all five build projects |
| `621b5aae` | `tests/ReliefModifierTest.cpp` (tests 1–8, 10) |
| `547c417d` | `cc_relief_modifier` + `relief_sphere_no_uv` |
| `0ea40138` | hygiene opt-out for the two non-finite test fixtures |

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

**Tests.** `ReliefModifierTest` 59 checks, 0 failures.  Gate suites, all
run on the final tree: `GlintModifierTest` ALL PASSED,
`HairTangentPlumbingTest` 123/0, `SurfaceCurvatureTest` 94/0,
`CstResolverTest` 44/0, `CstRecordDeriveTest` 23/0,
`CstIncrementalSafetyTest` 38/0, `CstSourceInstanceTest` 455/0,
`ScalarPainterParserTest` 60/0, `SourceHygieneTest` 164/0,
`BDPTVertexRIGRebuildTest` 15/0.  `ObjectMirrorTest` is 161 passed / **1
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

That last figure **does not confirm §10's "~4× one albedo evaluation"**;
it is smaller, and the honest reading is that the comparison is not
apples-to-apples — the albedo slot is evidently queried more than once
per hit on this Lambertian + NEE path, so `B−A` is not "one evaluation".
What is directly verifiable is the count: the modifier performs **exactly
four** height evaluations per hit on an object that binds it, and zero on
objects that do not.  §10's claim should be read as that count, not as a
measured 4× wall-clock ratio.

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
   NormalMap and BumpMap gate the projection on `ri.bHasShadingTangent`
   while GlintModifier runs it unconditionally, and the two are
   **observably different** — for a hit with `bHasShadingTangent == false`
   the gated form rebuilds with `CreateFromW` (an arbitrary canonical-axis
   `u`) and the unconditional form projects the previous `u`, giving
   different `u`/`v` about the same `w`.  Folding either into the helper
   would have changed one of the three modifiers' behaviour, so the gate
   stayed at each call site and all three are byte-identical to before.
   ReliefModifier follows the BumpMap/NormalMap gate (it is a
   height-gradient tilt, the same family).  Verified green:
   `GlintModifierTest` (incl. its test 5 "tangent direction preserved
   against a NON-canonical base tangent" and test 11 handedness) and
   `HairTangentPlumbingTest`'s four bump/normal-map cases including
   "non-hair hit byte-matches legacy CreateFromW rebuild".
4. **One guard the design did not name**: a `mag2 > 1e-12 && isfinite`
   bail on the perturbed vector, for a gradient large enough to cancel
   `N`.  §3.2's "no NaN normal ever reaches a material" implies it;
   without it `Normalize` would receive a zero vector.  It is a bail, not
   a clamp — no flat spot, and still no geometric-horizon clamp.
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
